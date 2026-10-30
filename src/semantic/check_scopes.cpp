#include "checker.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Scope management
// ==========================================================================

/// Opens a new innermost lexical scope for value bindings.
auto checker::push_scope() -> void { function_.scopes_.emplace_back(); }

/// Closes the innermost lexical scope.
auto checker::pop_scope() -> void { function_.scopes_.pop_back(); }

/// Introduces or shadows `name` in the innermost scope. A no-op for an
/// empty name (e.g. `_`) or when no scope is open.
auto checker::bind_value(std::string_view name, type_id type,
                         binding_origin origin, source_span span) -> void {
  if (name.empty() || function_.scopes_.empty()) {
    return;
  }
  function_.scopes_.back().insert_or_assign(
      std::string(name),
      value_binding{.type = type, .origin = origin, .span = span});
}

/// Looks up `name` from the innermost scope outward, returning the first
/// (most-shadowing) match, or `nullptr` if unbound — or if reaching that
/// match would cross a lambda whose explicit capture list does not name
/// it, which makes the name genuinely unreachable from here.
auto checker::lookup_value(std::string_view name) -> const value_binding * {
  const auto found = find_value_in_scopes(name);
  if (found.blocked_by != nullptr) {
    return nullptr;
  }
  if (found.binding != nullptr) {
    note_capture_use(name, found.depth);
    for (auto &boundary : function_.task_boundaries_) {
      if (boundary.scope_floor > found.depth) {
        boundary.captured.emplace_back(std::string(name), found.binding->type);
      }
    }
  }
  return found.binding;
}

/// Records that `name`, bound at scope index `depth`, was actually read
/// from inside every lambda whose boundary the reference crossed. What is
/// left unmarked when the body finishes is a capture the body never used,
/// which is worth telling the programmer about.
auto checker::note_capture_use(std::string_view name, size_t depth) -> void {
  for (auto &barrier : function_.capture_barriers_) {
    if (barrier.scope_floor > depth && barrier.allowed.has_value()) {
      barrier.used.emplace(name);
    }
  }
}

auto checker::find_value_in_scopes(std::string_view name) -> scope_lookup {
  const auto key = std::string(name);
  for (size_t depth = function_.scopes_.size(); depth > 0; --depth) {
    const auto &scope = function_.scopes_[depth - 1];
    const auto it = scope.find(key);
    if (it == scope.end()) {
      continue;
    }
    // A binding from outside an inline `static for` read by its yield makes
    // that yield run-time code.
    for (const auto &[decl, floor] : cache_.static_for_yields_) {
      if (depth - 1 < floor) {
        cache_.static_for_reads_[decl].runtime = true;
      }
    }
    // The match sits at scope index `depth - 1`; every barrier opened
    // above that index is a lambda boundary this reference crosses.
    // Report the innermost one that refuses the name, so the diagnostic
    // points at the capture list the programmer most likely meant to edit.
    for (const auto &barrier : function_.capture_barriers_) {
      if (barrier.scope_floor <= depth - 1 || !barrier.allowed.has_value()) {
        continue;
      }
      if (!barrier.allowed->contains(key)) {
        return scope_lookup{
            .binding = &it->second, .blocked_by = &barrier, .depth = depth - 1};
      }
    }
    return scope_lookup{
        .binding = &it->second, .blocked_by = nullptr, .depth = depth - 1};
  }
  return scope_lookup{};
}

/// The capture list that puts `name` out of reach, or `nullptr`. Used to
/// replace the generic "undefined name" diagnostic with one that explains
/// the real cause.
auto checker::capture_barrier_blocking(std::string_view name)
    -> const capture_barrier * {
  return find_value_in_scopes(name).blocked_by;
}

/// Reports a reference to an outer name a capture list left out. Separate
/// from `emit_undefined_name` because the cause and the fix are different:
/// the name is perfectly well defined, it is just not reachable from
/// inside this lambda.
auto checker::emit_capture_not_listed(source_span span, std::string_view name,
                                      const capture_barrier &barrier) -> void {
  if (!function_.reported_undefined_.insert(std::format("capture:{}", name))
           .second) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("`{}` is not in this lambda's capture list", name),
                 file_.file_id_);
  diag.with_label(span, std::format("`{}` is not reachable here", name));
  if (barrier.lambda != nullptr) {
    diag.with_secondary_label(
        barrier.lambda->captures_span,
        barrier.allowed->empty()
            ? "this lambda captures nothing, so no outer name is in scope"
            : std::format(
                  "this capture list is the complete set of outer names "
                  "this lambda can reach: {}",
                  format_capture_names(*barrier.allowed)));
  }
  diag.with_help(std::format(
      "Add `{}` to the capture list, or drop the list entirely to capture "
      "every outer name the body uses. Module-level functions and `static` "
      "bindings are always in scope and never need listing.",
      name));
  emit_diag(diag);
  mark_error();
}

auto checker::format_capture_names(
    const std::unordered_map<std::string, ast::capture_mode> &allowed)
    -> std::string {
  auto names = std::vector<std::string>{};
  names.reserve(allowed.size());
  for (const auto &[key, mode] : allowed) {
    names.push_back(std::format("`{}`", key));
    (void)mode;
  }
  std::ranges::sort(names);
  auto out = std::string{};
  for (const auto &entry : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += entry;
  }
  return out;
}

/// Opens a new generic-parameter scope, interning each named parameter as
/// a `type_param` type — except a value parameter the const-generic
/// instance currently being checked has a constant for, which interns as
/// that constant instead (`session_.const_param_slots_`). That one substitution
/// is the whole of monomorphization on the checker's side: with `n` bound to
/// `3` rather than to itself, every type the body mentions resolves the way
/// it would have if the programmer had written `3` there, and the code that
/// needs a real number (`index[n]`'s bound, `try_from`'s check) finds one.
auto checker::push_type_params(const std::vector<ast::type_param> &params)
    -> void {
  auto scope = type_scope{};
  for (const auto &param : params) {
    if (param.name.empty()) {
      continue;
    }
    if (const auto bound = session_.const_param_slots_.find(param.name);
        param.is_value_param && bound != session_.const_param_slots_.end()) {
      scope.emplace(param.name, bound->second);
      continue;
    }
    // The type-parameter analog of `session_.const_param_slots_`: while
    // checking a monomorphized copy of a generic method
    // (`instantiate_hk_method`), each of its *type* parameters is bound to the
    // concrete type the call solved for it instead of an abstract `type_param`.
    if (const auto bound = session_.type_param_slots_.find(param.name);
        !param.is_value_param && bound != session_.type_param_slots_.end()) {
      scope.emplace(param.name, bound->second);
      continue;
    }
    scope.emplace(param.name,
                  session_.types_.type_param(
                      param.name, param.higher_kinded_arity, &param));
  }
  function_.type_params_.push_back(std::move(scope));
}

/// Closes the innermost generic-parameter scope.
auto checker::pop_type_params() -> void { function_.type_params_.pop_back(); }

/// Binds each compile-time *value* parameter (`n: usize`) as an ordinary
/// value in the current scope. A value parameter has two lives: it is a
/// type argument to `resolve_type` (so `vec[T, n]` means something) and a
/// plain value to `infer_expr` (so `self < n` and `for i in 0..n` mean
/// something). This is the second one.
auto checker::bind_value_params(const std::vector<ast::type_param> &params)
    -> void {
  for (const auto &param : params) {
    if (!param.is_value_param || param.name.empty()) {
      continue;
    }
    const auto type =
        param.bound_or_type != nullptr
            ? resolve_type(*param.bound_or_type, current_resolve_ctx())
            : k_unknown_type;
    bind_value(param.name, type, binding_origin::parameter, param.span);
  }
}

/// Whether any generic parameter currently in scope is still *abstract* —
/// an unsubstituted `type_param_kind` rather than a concrete type some
/// instantiation bound it to.
///
/// This is the checker's test for "the body being checked is a template,
/// not an instance": a generic `def`, `impl` or `extend` body is checked
/// once, against its bounds, and its instances are made from its records
/// by substitution (`substitute_instance`). It says only whose records a
/// body's are; no elaboration decision is gated on it — a site that needs
/// its inputs concrete asks them (`mentions_template_param`) and defers to
/// the instances if not.
auto checker::in_abstract_type_param_scope() -> bool {
  for (const auto &scope : function_.type_params_) {
    for (const auto &[name, id] : scope) {
      if (session_.types_.entry(id).kind == type_kind::type_param_kind) {
        return true;
      }
    }
  }
  return false;
}

/// Looks up an in-scope generic parameter by name, searching from the
/// innermost scope outward.
auto checker::lookup_type_param(std::string_view name)
    -> std::optional<type_id> {
  for (auto &type_param : std::views::reverse(function_.type_params_)) {
    if (const auto it = type_param.find(std::string(name));
        it != type_param.end()) {
      return it->second;
    }
  }
  return std::nullopt;
}

// ==========================================================================
//  Module member lookup (across every file of the current module, plus
//  imports and the prelude).
// ==========================================================================

/// Finds the session module that owns the longest matching prefix of
/// `path`, used to resolve a multi-segment named-type path like
/// `pkg.mod.Thing` to the module `pkg.mod` and member `Thing`.
auto checker::find_session_module_of_path(
    const std::vector<std::string> &path) const -> const module_members * {
  // Longest module prefix registered in the session wins.
  const module_members *found = nullptr;
  for (size_t i = path.size(); i >= 1; --i) {
    const auto prefix = join_module_path_prefix(path, i);
    if (const auto *members = index_.find_module(prefix)) {
      found = members;
      break;
    }
    if (i == 1) {
      break;
    }
  }
  return found;
}

/// `find_session_module_of_path` for a path read as *absolute*, which is
/// only a reading at all when the path's root is visible here without an
/// import (`root_visible_without_import` — the same rule `resolution.cpp`
/// validates paths with, so the checker never resolves a path the
/// validator would reject). A path read through an import alias is
/// rewritten to its absolute form first and does not come through here.
auto checker::find_visible_module_of_path(
    const std::vector<std::string> &path) const -> const module_members * {
  if (path.empty() ||
      !root_visible_without_import(visibility_module(), path.front())) {
    return nullptr;
  }
  return find_session_module_of_path(path);
}

/// The module a *relative* qualified path names: `inner.holder` written
/// inside `module main` means `main.inner`'s `holder`, which is how a
/// submodule declared in the same file is meant to be reachable from its
/// parent (`resolution.cpp`'s `resolve_named_type_path` has always
/// accepted this spelling; only the checker did not, so the reference
/// typed as unknown and the failure surfaced from lowering).
///
/// Unlike `find_session_module_of_path` this requires the *whole* prefix
/// to name a module, rather than taking the longest one that matches:
/// falling back to a shorter prefix here would make `bogus.holder`
/// silently resolve to the current module's own `holder`.
///
/// Always tried *after* imports and the absolute reading — after
/// `use pkg.inner`, `inner.holder` is the imported module's, not a
/// submodule of this one (see `test_qualified_type_path_through_import`).
/// A leading `super` chain steps up to the parent module first, so a
/// submodule can name a sibling (`super.a.holder` from inside `main.b`) —
/// the same reading `resolution.cpp`'s `resolve_named_type_path` gives it.
auto checker::find_submodule_of_current_module(
    const std::vector<std::string> &path) const -> const module_members * {
  if (file_.module_ == nullptr || path.empty()) {
    return nullptr;
  }
  auto base = file_.module_->module_name;
  auto first = size_t{0};
  while (first < path.size() && path[first] == "super") {
    base = parent_module_name(base);
    if (base.empty()) {
      return nullptr; // `super` from a root module has no parent
    }
    ++first;
  }
  // Without a `super` prefix the path needs at least one segment naming a
  // module plus the member itself; with one, the parent module alone can
  // be the owner (`super.helper`).
  if (path.size() - first < (first == 0 ? 2 : 1)) {
    return nullptr;
  }
  auto qualified = base;
  for (size_t i = first; i + 1 < path.size(); ++i) {
    qualified += '.';
    qualified += path[i];
  }
  return index_.find_module(qualified);
}

/// The module that owns the member `path` names, where "owns" is decided
/// by `owns` — trying the three readings `find_type_decl_by_path` tries, in
/// the same order: a leading import alias, the absolute path, then relative
/// to the module being checked. Returns null when no reading names an
/// owning module, so the caller can fall through to the existing
/// "validated elsewhere" behavior rather than reporting anything here.
template <typename OwnsFn>
auto checker::find_member_owner_of_path(const std::vector<std::string> &path,
                                        const OwnsFn &owns) const
    -> const module_members * {
  if (path.size() < 2) {
    return nullptr;
  }
  if (const auto *binding = find_import(path.front());
      binding != nullptr && binding->leaf_name.empty()) {
    const module_members *aliased =
        index_.find_module(join_strings(binding->path, "."));
    if (aliased == nullptr) {
      aliased = import_source_module(*binding);
    }
    if (aliased != nullptr) {
      auto absolute = split_module_name(aliased->module_name);
      absolute.insert(absolute.end(), path.begin() + 1, path.end());
      if (const auto *owner = find_session_module_of_path(absolute);
          owns(owner)) {
        return owner;
      }
    }
  }
  if (const auto *owner = find_visible_module_of_path(path); owns(owner)) {
    return owner;
  }
  if (const auto *owner = find_submodule_of_current_module(path); owns(owner)) {
    return owner;
  }
  return nullptr;
}

/// The module whose `static` bindings include the one `path` names.
auto checker::find_static_owner_of_path(
    const std::vector<std::string> &path) const -> const module_members * {
  return find_member_owner_of_path(
      path, [&path](const module_members *members) -> bool {
        return members != nullptr && members->statics.contains(path.back());
      });
}

/// The module whose functions include the one `path` names — the call-free
/// counterpart of the callee resolution a qualified *call* goes through, so
/// `a.b.f` written as a plain value finds the same declaration.
auto checker::find_fn_owner_of_path(const std::vector<std::string> &path) const
    -> const module_members * {
  return find_member_owner_of_path(
      path, [&path](const module_members *members) -> bool {
        return members != nullptr && members->functions.contains(path.back());
      });
}

/// Whether `path`'s first segment names (or prefixes) a module declared
/// somewhere in this session — used to tell a session-owned reference
/// apart from a genuinely external one this checker can't see into.
auto checker::session_owns_path_root(const std::vector<std::string> &path) const
    -> bool {
  if (path.empty()) {
    return false;
  }
  const auto &root = path.front();
  return std::ranges::any_of(
      index_.modules, [&root](const auto &entry) -> bool {
        const auto &name = entry.first;
        return name == root || name.starts_with(root + ".");
      });
}

/// Returns the `use` bindings recorded for the file currently being
/// checked, or `nullptr` if it has none.
auto checker::imports_for_current_file() const
    -> const std::vector<import_binding> * {
  const auto it = index_.imports.find(file_.file_id_);
  return it != index_.imports.end() ? &it->second : nullptr;
}

/// The import that brings in something originally named `name` under a
/// different local name (`use util as u`, `use a.{b as c}`), for the
/// undefined-name diagnostic to point at.
auto checker::find_renamed_import(std::string_view name) const
    -> const import_binding * {
  const auto *imports = imports_for_current_file();
  if (imports == nullptr) {
    return nullptr;
  }
  for (const auto &binding : *imports) {
    if (binding.is_wildcard || binding.local_name == name) {
      continue;
    }
    const auto &original = binding.leaf_name.empty() && !binding.path.empty()
                               ? binding.path.back()
                               : binding.leaf_name;
    if (original == name) {
      return &binding;
    }
  }
  return nullptr;
}

/// Finds the non-wildcard import binding that introduces `name` locally
/// in the current file.
auto checker::find_import(std::string_view name) const
    -> const import_binding * {
  const auto *imports = imports_for_current_file();
  if (imports == nullptr) {
    return nullptr;
  }
  for (const auto &binding : *imports) {
    if (!binding.is_wildcard && binding.local_name == name) {
      return &binding;
    }
  }
  return nullptr;
}

/// The session-owned modules whose members a wildcard import
/// (`use a.b.*`) in the current file makes available for unqualified
/// lookup. Recomputed per call because `file_.file_id_` swaps temporarily while
/// checking a callee declared in another file. External-rooted wildcards
/// never resolve here — they only suppress undefined-name errors
/// (`file_.file_has_external_wildcard_`).
auto checker::wildcard_import_sources() const
    -> std::vector<const module_members *> {
  auto sources = std::vector<const module_members *>{};
  const auto *imports = imports_for_current_file();
  if (imports == nullptr) {
    return sources;
  }
  for (const auto &binding : *imports) {
    if (!binding.is_wildcard) {
      continue;
    }
    if (const auto *source =
            index_.find_module(join_strings(binding.path, "."))) {
      sources.push_back(source);
    }
  }
  return sources;
}

/// The module that an import binding pulls its member from, when the
/// import stays within this compilation session.
auto checker::import_source_module(const import_binding &binding) const
    -> const module_members * {
  if (!session_owns_path_root(binding.path)) {
    return nullptr;
  }
  if (!binding.leaf_name.empty()) {
    return index_.find_module(join_strings(binding.path, "."));
  }
  // `use a.b.c` — `c` may itself be a module or a member of `a.b`.
  if (index_.find_module(join_strings(binding.path, ".")) != nullptr) {
    return nullptr; // imports a module; members are accessed via paths
  }
  if (binding.path.size() < 2) {
    return nullptr;
  }
  auto parent = binding.path;
  parent.pop_back();
  return index_.find_module(join_strings(parent, "."));
}

/// Returns the local member name an import binds — the selector's item
/// name if present, otherwise the trailing path segment.
auto checker::imported_member_name(const import_binding &binding) const
    -> std::string {
  return binding.leaf_name.empty() ? binding.path.back() : binding.leaf_name;
}

// ==========================================================================
//  Identifier resolution (value namespace)
// ==========================================================================

/// Whether `name` is a builtin, prelude container/trait, or a
/// type/trait/concept the current module declares — used to avoid
/// reporting an "undefined name" error for a name that is really a type
/// used where the checker didn't expect one.
auto checker::is_type_like_name(std::string_view name) -> bool {
  if (is_builtin_scalar_name(name) || builtin_generic_arity(name) ||
      name == "array" || find_prelude_trait(name).has_value()) {
    return true;
  }
  if (file_.module_ != nullptr &&
      (file_.module_->types.contains(std::string(name)) ||
       file_.module_->traits.contains(std::string(name)) ||
       file_.module_->concepts.contains(std::string(name)))) {
    return true;
  }
  return false;
}

/// Builds the candidate name list for "did you mean" suggestions on an
/// undefined value name: every in-scope binding, plus the current
/// module's functions, statics, and sum-type variants, plus common
/// prelude functions.
auto checker::value_name_candidates() -> std::vector<std::string> {
  auto candidates = std::vector<std::string>{};
  for (const auto &scope : function_.scopes_) {
    for (const auto &[name, binding] : scope) {
      candidates.push_back(name);
    }
  }
  if (file_.module_ != nullptr) {
    for (const auto &[name, decl] : file_.module_->functions) {
      candidates.push_back(name);
    }
    for (const auto &[name, decl] : file_.module_->statics) {
      candidates.push_back(name);
    }
    for (const auto &[name, decl] : file_.module_->variants) {
      candidates.push_back(name);
    }
  }
  for (const auto prelude :
       {"println", "print", "panic", "assert", "size_of", "align_of",
        "needs_drop", "ptr_cast", "args", "env", "min", "max",
        "slice_from_raw_parts", "slice_mut_from_raw_parts"}) {
    candidates.emplace_back(prelude);
  }
  return candidates;
}

/// Emits "undefined name" once per distinct name per file, with a
/// suggestion or a define/import hint.
auto checker::emit_undefined_name(source_span span, std::string_view name)
    -> void {
  if (!function_.reported_undefined_.insert(std::format("value:{}", name))
           .second) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("undefined name `{}`", name), file_.file_id_);
  diag.with_label(span, "not found in this scope");
  if (const auto *renamed = find_renamed_import(name)) {
    diag.with_help(std::format(
        "This file imports `{}` under the name `{}`, and a renamed import "
        "is only reachable by its new name. Write `{}` here.",
        name, renamed->local_name, renamed->local_name));
  } else if (const auto suggestion =
                 best_suggestion(name, value_name_candidates())) {
    diag.with_help(std::format("did you mean `{}`?", *suggestion));
  } else {
    diag.with_help(std::format(
        "Define `{}` before using it, or import it with `use`.", name));
  }
  emit_diag(diag);
  mark_error();
}

/// Names the checker treats as always resolvable without deeper lookup:
/// prelude functions, execution-context names, and concurrency primitives
/// whose full typing is out of scope for now.
///
/// `break` and `continue` used to be listed here, back when they were not
/// tokenized as keywords and so arrived as ordinary identifiers. That made
/// them type-check as unknown and then fail in lowering with an internal
/// "no concrete checked type" message. They are real statements now.
auto checker::is_prelude_value_name(std::string_view name) -> bool {
  return name == "println" || name == "print" || name == "panic" ||
         name == "assert" || name == "size_of" || name == "align_of" ||
         name == "needs_drop" || name == "ptr_cast" || name == "args" ||
         name == "env" || name == "min" || name == "max" || name == "cancel" ||
         name == "pool" || name == "io" || name == "cpu" || name == "channel" ||
         name == "watch" || name == "expr" || name == "slice_from_raw_parts" ||
         name == "slice_mut_from_raw_parts";
}

auto checker::find_module_scope_value(std::string_view name) const
    -> std::optional<module_value_ref> {
  const auto in =
      [&](const module_members &members,
          const std::string &member) -> std::optional<module_value_ref> {
    if (const auto it = members.functions.find(member);
        it != members.functions.end()) {
      return module_value_ref{.function = it->second.decl,
                              .static_binding = nullptr,
                              .owner = &members};
    }
    if (const auto it = members.statics.find(member);
        it != members.statics.end()) {
      return module_value_ref{.function = nullptr,
                              .static_binding = it->second.decl,
                              .owner = &members};
    }
    return std::nullopt;
  };
  const auto key = std::string(name);
  if (file_.module_ != nullptr) {
    if (auto found = in(*file_.module_, key)) {
      return found;
    }
  }
  if (const auto *binding = find_import(name)) {
    if (const auto *source = import_source_module(*binding)) {
      return in(*source, imported_member_name(*binding));
    }
    return std::nullopt;
  }
  for (const auto *source : wildcard_import_sources()) {
    if (auto found = in(*source, key)) {
      return found;
    }
  }
  return std::nullopt;
}

/// Resolves a value-position identifier through, in order: a variant
/// constructor (if `@`-prefixed); a local binding or `self`; the current
/// module's functions/statics; an imported name; a prelude/type-like name;
/// a bare (un-`@`-prefixed) variant spelling; or reports undefined-name,
/// unless suppressed by an external wildcard import.
auto checker::resolve_ident(const ast::ident_expr &ident, type_id expected)
    -> type_id {
  const auto &name = ident.name;
  if (name.empty() || ident.has_error) {
    return k_unknown_type;
  }

  if (is_variant_ident(ident)) {
    if (const auto variant =
            resolve_variant_value(name, nullptr, ident.span, expected)) {
      return *variant;
    }
    emit_undefined_variant(ident.span, name, expected);
    return k_error_type;
  }

  // `return` inside a contract condition (the parser only ever produces an
  // identifier spelled `return` there) names the value the function
  // returns. It exists in a `post`, where the function has produced one,
  // and nowhere else.
  if (name == "return" && function_.in_contract_) {
    if (!function_.in_postcondition_) {
      error_with_help(
          ident.span,
          "`return` names the value the function returns, so it may only "
          "appear in a `post` condition",
          "there is no returned value here",
          "A `pre` condition runs on entry, before the function has produced "
          "anything, and a type `invariant` describes a value that has no "
          "call in progress at all. Move this to a `post` condition if it "
          "describes the result.");
      return k_error_type;
    }
    if (session_.types_.is_unit(function_.return_type_)) {
      error_with_help(
          ident.span,
          "`return` names the value the function returns, and this function "
          "returns `unit`",
          "no meaningful value to constrain",
          "A `unit`-returning function has only one possible result, so a "
          "`post` condition about it can never say anything. Constrain a "
          "parameter instead, or give the function a return type.");
      return k_error_type;
    }
    return function_.return_type_;
  }

  if (const auto *binding = lookup_value(name)) {
    if (binding->generic_lambda != nullptr) {
      return generic_lambda_value(
          {.node = &ident, .name = ident.name, .span = ident.span}, *binding,
          expected);
    }
    if (binding->origin == binding_origin::parameter) {
      record_const_param_reference(ident, binding->type);
    }
    return binding->type;
  }
  // Checked before every module-level fallback below: a local an enclosing
  // capture list left out must be reported as *that*, not silently
  // re-resolved to a same-named module item or fluffed into `unknown` by
  // one of the resilience paths further down.
  if (const auto *barrier = capture_barrier_blocking(name)) {
    emit_capture_not_listed(ident.span, name, *barrier);
    return k_error_type;
  }
  if (name == "self" && function_.self_type_ != k_unknown_type) {
    return function_.self_type_;
  }

  if (const auto value = find_module_scope_value(name)) {
    if (value->function != nullptr) {
      record_fn_value_reference(ident, *value->function,
                                value->owner->module_name);
      return fn_type_of(*value->function, value->owner);
    }
    const auto type = static_binding_type(*value->static_binding, value->owner);
    record_static_const_reference(ident, *value->static_binding, type,
                                  value->owner->module_name);
    return type;
  }
  if (find_import(name) != nullptr) {
    return k_unknown_type;
  }

  if (is_prelude_value_name(name) || is_type_like_name(name)) {
    return k_unknown_type;
  }

  // Bare variant spelling without `@`: typed as the variant so the error
  // doesn't cascade, but rejected — variants are always spelled `@name`.
  if (const auto variant =
          resolve_variant_value(name, nullptr, ident.span, expected)) {
    emit_bare_variant(ident.span, name, /*is_call=*/false);
    return *variant;
  }

  if (file_.file_has_external_wildcard_) {
    return k_unknown_type;
  }

  emit_undefined_name(ident.span, name);
  return k_error_type;
}

/// Rejects a variant spelled without its `@` (`some(5)`, `none`). The
/// checker can tell what was meant, so it says so and shows the fix.
auto checker::emit_bare_variant(source_span span, std::string_view name,
                                bool is_call) -> void {
  error_with_help(
      span, std::format("variant `{}` must be written `@{}`", name, name),
      "missing `@`",
      std::format("sum-type variants are always spelled with a leading "
                  "`@`, which keeps them apart from functions and values "
                  "of the same name. Write `@{}{}` here.",
                  name, is_call ? "(...)" : ""));
}

/// Emits "unknown variant" once per distinct name per file, noting the
/// expected sum type's actual variants when one is known.
auto checker::emit_undefined_variant(source_span span, std::string_view name,
                                     type_id expected) -> void {
  if (!function_.reported_undefined_.insert(std::format("variant:{}", name))
           .second) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("unknown variant `@{}`", name), file_.file_id_);
  diag.with_label(span, "no sum type in scope declares this variant");
  const auto expected_entry = session_.types_.entry(strip_refs(expected));
  if (const auto *variants = sum_variants_of(expected_entry)) {
    auto names = std::string{};
    for (const auto &variant : *variants) {
      if (!names.empty()) {
        names += ", ";
      }
      names += std::format("`@{}`", variant.name);
    }
    diag.with_note(std::format("`{}` declares the variants {}",
                               expected_entry.name, names));
  }
  emit_diag(diag);
  mark_error();
}

/// Resolves and caches a `static` binding's type, guarding against an
/// initializer cycle (`static a = b` / `static b = a`) by returning
/// `unknown` if the declaration is already being resolved.
///
/// An unannotated binding's type is its initializer's, so a reference
/// reached before the declaration itself is checked (an earlier function,
/// another module) checks the declaration on demand
/// (`check_static_binding_early`) rather than seeing `unknown`.
auto checker::static_binding_type(const ast::static_decl &decl,
                                  const module_members *owner) -> type_id {
  if (const auto it = cache_.static_types_.find(&decl);
      it != cache_.static_types_.end()) {
    return it->second;
  }
  if (decl.type_annotation == nullptr && decl.initializer != nullptr &&
      !decl.initializer->has_error &&
      !cache_.statics_in_progress_.contains(&decl)) {
    check_static_binding_early(decl, owner);
    if (const auto it = cache_.static_types_.find(&decl);
        it != cache_.static_types_.end()) {
      return it->second;
    }
  }
  if (!cache_.statics_in_progress_.insert(&decl).second) {
    return k_unknown_type; // initializer cycle
  }
  auto type = k_unknown_type;
  if (decl.type_annotation != nullptr) {
    type = resolve_type(*decl.type_annotation,
                        resolve_ctx{.module = owner,
                                    .param_bindings = nullptr,
                                    .use_type_param_stack = false,
                                    .quiet = true});
  }
  cache_.statics_in_progress_.erase(&decl);
  cache_.static_types_.emplace(&decl, type);
  return type;
}

/// Checks an unannotated `static let` ahead of its place in file order, as
/// its declaration would be: in its own module and file, with none of the
/// body being checked right now in scope. `check_static_decl` then skips
/// it when file order reaches it.
auto checker::check_static_binding_early(const ast::static_decl &decl,
                                         const module_members *owner) -> void {
  const auto &declaring = owner != nullptr ? *owner : *file_.module_;
  const auto listed = declaring.statics.find(decl.name);
  const auto file =
      listed != declaring.statics.end() && listed->second.decl == &decl
          ? listed->second.file_id
          : file_.file_id_;
  auto module_level = body_context{};
  swap_body_context(module_level);
  // `in_module_context` returns its body's result, so the body needs one.
  [[maybe_unused]] const auto checked =
      in_module_context(declaring, file, [&] -> bool {
        push_scope();
        check_static_decl(decl);
        pop_scope();
        return true;
      });
  swap_body_context(module_level);
  cache_.early_checked_static_bindings_.insert(&decl);
}

/// Ensures `decl`'s compile-time value is evaluated and bound in
/// `comptime_eval_`, then returns it — or `nullptr` if there's no
/// initializer, it already failed to check, or evaluation itself
/// failed. Skips re-evaluating (and potentially re-diagnosing) an
/// initializer a forward reference already resolved, exactly like
/// `check_static_decl`'s own `binding` case (which is in fact one of
/// this helper's two callers, refactored to share it) — `resolve_ident`
/// is the other, since a name's *first* reference in file-checking
/// order might be an ordinary expression far from the `static let`'s
/// own declaration, and every reference needs the same confluent,
/// order-independent evaluation the rest of this compile-time subsystem
/// already guarantees.
///
/// `owner` is the module declaring `decl` when that may not be the module
/// being checked. The initializer is evaluated in *its* module's context —
/// module, file, and imports — so a dotted name inside it
/// (`classify_dotted_root`) reads the way it would at the declaration, not
/// at whichever reference happened to reach it first.
auto checker::ensure_static_binding_evaluated(const ast::static_decl &decl,
                                              const module_members *owner)
    -> const comptime::value * {
  if (decl.initializer == nullptr || decl.initializer->has_error ||
      decl.name.empty()) {
    return nullptr;
  }
  // Memoized per *declaration*, never by bare name: a parent module and
  // an inline submodule may each declare `static limit`. Compile-time
  // code reaches this through the checker's own name lookup
  // (`read_bare_name_for_comptime`, `read_dotted_path_for_comptime`).
  if (const auto it = cache_.static_binding_values_.find(&decl);
      it != cache_.static_binding_values_.end()) {
    if (cache_.failed_static_bindings_.contains(&decl)) {
      mark_error();
    }
    return it->second.has_value() ? &*it->second : nullptr;
  }
  // The initializer runs on the VM, which lowers it from what checking it
  // recorded, so a reference reached before file order checks the
  // declaration checks it now. Checking it evaluates it too.
  if (bodies_phase_ && !session_.node_types_.contains(decl.initializer.get()) &&
      !cache_.statics_in_progress_.contains(&decl) &&
      !cache_.early_checked_static_bindings_.contains(&decl) &&
      !cache_.static_bindings_evaluating_.contains(&decl)) {
    check_static_binding_early(decl, owner);
    if (const auto it = cache_.static_binding_values_.find(&decl);
        it != cache_.static_binding_values_.end()) {
      // A failed initializer was reported in its own file; the file reading
      // it must not be lowered either.
      if (cache_.failed_static_bindings_.contains(&decl)) {
        mark_error();
      }
      return it->second.has_value() ? &*it->second : nullptr;
    }
  }
  // A dotted path reaches this through the checker rather than the
  // evaluator's own by-name cycle guard (`resolve_pending_static`), so a
  // self-referential `static a = a.x` needs a guard here too.
  if (!cache_.static_bindings_evaluating_.insert(&decl).second) {
    return nullptr;
  }
  const auto errors_before = diag_.error_count();
  const auto evaluated = [&] -> comptime::value {
    if (owner == nullptr || owner == file_.module_) {
      return comptime_eval_.evaluate(*decl.initializer);
    }
    const auto listed = owner->statics.find(decl.name);
    return in_module_context(
        *owner,
        listed != owner->statics.end() ? listed->second.file_id
                                       : file_.file_id_,
        [&] -> comptime::value {
          return comptime_eval_.evaluate(*decl.initializer);
        });
  }();
  cache_.static_bindings_evaluating_.erase(&decl);
  if (evaluated.is_error()) {
    cache_.static_binding_values_.emplace(&decl, std::nullopt);
    // The initializer's own error already explains the failure. The file
    // being checked (the declaring one, or the one whose reference reached
    // the binding first) must not be lowered either: lowering would find
    // a reference with no value and report a misleading compiler gap.
    if (diag_.error_count() > errors_before) {
      cache_.failed_static_bindings_.insert(&decl);
      mark_error();
    }
    return nullptr;
  }
  return &*cache_.static_binding_values_.emplace(&decl, evaluated)
               .first->second;
}

/// If `decl` (the `static let` a name just resolved to) evaluates to a
/// materializable scalar, records `ident -> literal` in
/// `session_.static_const_values_` so `hir::lower_ident` can embed the value
/// directly instead of emitting an unresolvable `hir_local_ref` — see
/// `ensure_static_binding_evaluated`/`materialize_const_literal`'s doc
/// comments for why evaluation must happen here (not just at `decl`'s
/// own declaration site) and what "materializable" means. `type` is
/// `ident`'s already-resolved checked type (`static_binding_type`),
/// passed in rather than recomputed so the synthesized literal can be
/// given the exact same type in `session_.node_types_` — lowering requires
/// every node it visits, including one this pass synthesizes, to already have
/// a recorded checked type.
/// `reference` is the node the literal is substituted for: an
/// `ident_expr` for a bare name, or a `module_path_expr` for a
/// module-qualified one (`main.inner.limit`) — `static_const_values` is
/// keyed on `ast::node` precisely so both spellings can be recorded, and
/// `hir::lower_module_path` looks the path form up the same way
/// `hir::lower_ident` looks up the bare one.
/// Records that `reference` names `decl` — a module-level function used as
/// a plain value rather than called. Lowering reads this back (see
/// `checked_types::resolved_fn_values`) to build the reference's
/// `hir_local_ref` with the *declaring* module as its `owner_module`, which
/// is what both backends key their function tables on and what
/// `hir::find_reachable_modules` walks to decide a module must be compiled
/// at all. Without it a cross-module reference resolves against the
/// referencing module and finds nothing.
auto checker::record_fn_value_reference(const ast::expr &reference,
                                        const ast::func_decl &decl,
                                        std::string_view owner_module) -> void {
  session_.resolved_fn_values_[&reference] =
      resolved_callee{.decl = &decl,
                      .owner_module = std::string(owner_module),
                      .impl_target_type = "",
                      .receiver = nullptr};
}

auto checker::record_static_const_reference(const ast::expr &reference,
                                            const ast::static_decl &decl,
                                            type_id type,
                                            std::string_view owner_module)
    -> void {
  const auto *value =
      ensure_static_binding_evaluated(decl, index_.find_module(owner_module));
  if (value == nullptr) {
    return;
  }
  if (const auto *lit =
          materialize_const_literal(*value, reference.span, type)) {
    session_.static_const_values_[&reference] = lit;
    return;
  }
  if (value->kind == comptime::value_kind::struct_instance) {
    if (auto literal = build_static_struct_expr(*value, reference.span, type)) {
      session_.static_struct_values_[&reference] = literal.get();
      session_.synthesized_static_structs_.push_back(std::move(literal));
      return;
    }
  }
  if (const auto name = reify_static_global(decl, *value, type, owner_module)) {
    session_.static_global_refs_[&reference] = *name;
    return;
  }
  // Quoted syntax has no run-time form; compile-time code asks for it.
  if (value->fragment != nullptr) {
    session_.static_fragment_refs_[&reference] = &decl;
    return;
  }
  session_.unreadable_static_refs_[&reference] = decl.name;
}

/// A `static let` whose value has no single-literal materialization (an
/// array, a list, a tuple, a variant, a struct holding one of those) is
/// reified as real backing data: a global built once from its compile-time
/// value. Memoized per declaration in `session_.static_global_defs_` (mirroring
/// `cache_.static_binding_values_`) so a declaration referenced many times is
/// reified once; returns the assigned global name on success, `nullopt`
/// when some part of `value` has no run-time representation at its type
/// (see `reifiable`).
auto checker::reify_static_global(const ast::static_decl &decl,
                                  const comptime::value &value, type_id type,
                                  std::string_view owner_module)
    -> std::optional<std::string> {
  if (const auto it = session_.static_global_defs_.find(&decl);
      it != session_.static_global_defs_.end()) {
    return it->second.name;
  }
  auto reified = reify_value(value, type, decl.span);
  if (!reified.has_value()) {
    return std::nullopt;
  }
  auto name = std::format("static${}${}", decl.name,
                          session_.static_global_defs_.size());
  session_.static_global_defs_.emplace(
      &decl, lowering_results::static_global_def{
                 .name = name, .type = type, .value = std::move(*reified)});
  session_.static_global_owners_.emplace(name, std::string(owner_module));
  return name;
}

/// `value` as a `reified_value` of `type`: a number, boolean or string; an
/// array, tuple, struct or variant of such values; or a collection with a
/// `from_array` constructor holding them. `nullopt` when some part has no
/// run-time representation at its type. Each collection gets a synthesized
/// array literal whose `session_.array_literal_conversions_` entry names the
/// `from_array` instance for that collection's length — nothing in the
/// source spells those calls, so this is where they are requested.
auto checker::reify_value(const comptime::value &value, type_id type,
                          source_span span) -> std::optional<reified_value> {
  using shape = reified_value::shape;
  const auto entry =
      session_.types_.entry(type); // copy: types are interned below
  auto result = reified_value{.type = type};
  const auto reify_parts = [&](std::span<const comptime::value> items,
                               std::span<const type_id> item_types) -> bool {
    if (items.size() != item_types.size()) {
      return false;
    }
    for (size_t i = 0; i < items.size(); ++i) {
      auto part = reify_value(items[i], item_types[i], span);
      if (!part.has_value()) {
        return false;
      }
      result.parts.push_back(std::move(*part));
    }
    return true;
  };
  switch (value.kind) {
  case comptime::value_kind::integer:
  case comptime::value_kind::floating:
  case comptime::value_kind::boolean:
  case comptime::value_kind::string:
    if (entry.kind != type_kind::builtin_kind ||
        (value.kind == comptime::value_kind::string) != (entry.name == "str")) {
      return std::nullopt;
    }
    result.leaf = value;
    return result;
  case comptime::value_kind::struct_instance: {
    const auto *decl_fields = struct_fields_of(entry);
    if (decl_fields == nullptr || decl_fields->size() != value.fields.size()) {
      return std::nullopt;
    }
    result.kind = shape::structure;
    for (const auto &field : *decl_fields) {
      const auto found = value.fields.find(field.name);
      const auto field_type = struct_field_type(entry, field.name);
      if (found == value.fields.end() || !field_type.has_value()) {
        return std::nullopt;
      }
      auto part = reify_value(found->second, *field_type, span);
      if (!part.has_value()) {
        return std::nullopt;
      }
      result.names.push_back(field.name);
      result.parts.push_back(std::move(*part));
    }
    return result;
  }
  case comptime::value_kind::variant_instance: {
    // `option` and `result` have no declaration to read variants from; their
    // payloads are the generic arguments.
    if (entry.kind == type_kind::builtin_generic_kind &&
        ((entry.name == "option" && entry.args.size() == 1) ||
         (entry.name == "result" && entry.args.size() == 2))) {
      const auto &tag = value.variant_tag;
      const auto is_option = entry.name == "option";
      auto payload = std::vector<type_id>{};
      if (tag == (is_option ? "some" : "ok")) {
        payload.push_back(entry.args[0]);
      } else if (!is_option && tag == "err") {
        payload.push_back(entry.args[1]);
      } else if (!is_option || tag != "none") {
        return std::nullopt;
      }
      result.kind = shape::variant;
      result.names.push_back(tag);
      if (!reify_parts(value.elements, payload)) {
        return std::nullopt;
      }
      return result;
    }
    const auto *variant = find_variant(entry, value.variant_tag);
    if (variant == nullptr) {
      return std::nullopt;
    }
    result.kind = shape::variant;
    result.names.push_back(value.variant_tag);
    const auto payload = variant_payload_types(entry, *variant);
    if (!reify_parts(value.elements, payload)) {
      return std::nullopt;
    }
    return result;
  }
  case comptime::value_kind::list:
    break;
  default:
    return std::nullopt;
  }
  if (entry.kind == type_kind::tuple_kind) {
    result.kind = shape::tuple;
    if (!reify_parts(value.elements, entry.args)) {
      return std::nullopt;
    }
    return result;
  }
  if (entry.kind == type_kind::array_kind) {
    result.kind = shape::array;
    const auto element_types =
        std::vector<type_id>(value.elements.size(), entry.result);
    if (!reify_parts(value.elements, element_types)) {
      return std::nullopt;
    }
    return result;
  }
  auto literal = ast::make<ast::array_expr>();
  literal->span = span;
  auto target = type;
  const auto element = resolve_array_literal_conversion(*literal, target);
  if (!element.has_value() || mentions_type_var(target)) {
    session_.array_literal_conversions_.erase(literal.get());
    return std::nullopt;
  }
  const auto length = static_cast<uint64_t>(value.elements.size());
  session_.array_literal_conversions_[literal.get()].array_type =
      array_with_length(*element, session_.types_.const_value(
                                      session_.types_.usize_type(), length));
  instantiate_from_array_for(*literal, target, length);
  result.kind = shape::collection;
  result.conversion = literal.get();
  session_.synthesized_static_structs_.push_back(std::move(literal));
  const auto element_types =
      std::vector<type_id>(value.elements.size(), *element);
  if (!reify_parts(value.elements, element_types)) {
    return std::nullopt;
  }
  return result;
}

/// Records a *value* use of a value parameter (`return n`, `for i in 0..n`)
/// inside a const-generic instance as the constant that instance was made
/// for, the same way `record_static_const_reference` records a scalar
/// `static let`: `hir::lower_ident` finds the literal in
/// `session_.static_const_values_` and emits it instead of loading a local that
/// doesn't exist. It can't exist — a value parameter is passed at compile
/// time, so nothing at runtime holds `n`. In the template (where `n` is
/// still symbolic) `session_.const_param_values_` is empty and this does
/// nothing, which is correct: the template is never lowered.
auto checker::record_const_param_reference(const ast::ident_expr &ident,
                                           type_id type) -> void {
  const auto it = session_.const_param_values_.find(ident.name);
  if (it == session_.const_param_values_.end()) {
    // A template's own value parameter: each instance has its constant.
    if (function_.current_template_ != nullptr) {
      const auto abstract =
          template_abstract_params_.find(function_.current_template_);
      if (abstract != template_abstract_params_.end() &&
          abstract->second.contains(ident.name)) {
        defer_to_instances(
            ident, [this, &ident, type](instance_subst &subst) -> void {
              record_const_param_reference(*clone_of(subst, &ident),
                                           substitute_type(type, subst));
            });
      }
    }
    return;
  }
  auto lit = ast::make<ast::literal_expr>();
  lit->span = ident.span;
  lit->lit_kind = token_kind::int_lit;
  lit->value = std::to_string(it->second);
  const auto *raw = lit.get();
  session_.synthesized_const_literals_.push_back(std::move(lit));
  record_expr_type(*raw, type);
  session_.static_const_values_[&ident] = raw;
}

/// Synthesizes an `ast::literal_expr` embedding `value`'s exact
/// compile-time content — with `span` and `type` (recorded into
/// `session_.node_types_`, exactly like every other checked expression) taken
/// from the reference site being replaced, since the synthesized node
/// itself was never independently type-checked. Owned session-wide in
/// `session_.synthesized_const_literals_` (same stable-address
/// `vector<unique_ptr<T>>` pattern as `session_.synthesized_decls_`/
/// `session_.synthesized_types_`) — lets a scalar `static let` reference lower
/// as a real literal instead of an unresolvable `hir_local_ref` (see
/// `resolve_ident`). Deliberately narrow, mirroring `comptime::
/// try_eval_expr_builder_call`'s own `expr.lit` scope exactly: only
/// integer/floating/boolean values materialize; anything else (a
/// string, list, struct, ...) returns `nullptr`, leaving that reference
/// to fall back to the pre-existing (unresolvable-at-runtime) behavior
/// rather than risk mis-encoding something like a string's escape
/// sequences into a synthesized literal's raw spelling.
auto checker::materialize_const_literal(const comptime::value &value,
                                        source_span span, type_id type)
    -> const ast::literal_expr * {
  auto lit = build_const_literal(value, span, type);
  if (lit == nullptr) {
    return nullptr;
  }
  const auto *raw = lit.get();
  session_.synthesized_const_literals_.push_back(std::move(lit));
  return raw;
}

/// The owned literal `materialize_const_literal` hands out, for callers
/// (`build_static_struct_expr`) that embed it in a larger synthesized node
/// instead of keeping it in `session_.synthesized_const_literals_`.
auto checker::build_const_literal(const comptime::value &value,
                                  source_span span, type_id type)
    -> ast::ptr<ast::literal_expr> {
  auto lit = ast::make<ast::literal_expr>();
  lit->span = span;
  switch (value.kind) {
  case comptime::value_kind::integer:
    // `int_lit` token text is always an unsigned digit string — negative
    // values are ordinarily spelled as a `unary_expr(neg, ...)` wrapping
    // one (see `std.limits.min`'s own doc comment on this). A synthesized
    // literal has no such wrapper, so a negative `value.integer` is
    // written out as its 64-bit two's-complement bit pattern instead of a
    // signed decimal string — `encode_literal`/`compile_literal_value`
    // parse it back as raw bits and truncate to the target width exactly
    // as they would for any other literal, so the resulting value is
    // identical either way.
    lit->lit_kind = token_kind::int_lit;
    lit->value = std::to_string(static_cast<std::uint64_t>(value.integer));
    break;
  case comptime::value_kind::floating:
    lit->lit_kind = token_kind::float_lit;
    lit->value = std::to_string(value.floating);
    break;
  case comptime::value_kind::boolean:
    lit->lit_kind = value.boolean ? token_kind::kw_true : token_kind::kw_false;
    lit->value = value.boolean ? "true" : "false";
    break;
  case comptime::value_kind::string:
    // Re-encoded exactly as a parsed string literal would be spelled, so
    // lowering decodes it with the same `decode_string_literal` path.
    lit->lit_kind = token_kind::string_lit;
    lit->value = encode_string_literal(value.string);
    break;
  default:
    return nullptr;
  }
  record_expr_type(*lit, type);
  return lit;
}

/// Rebuilds a compile-time struct value as a struct literal whose fields
/// are literals (or nested struct literals), so a reference to a
/// struct-valued `static let` can be lowered as an ordinary struct
/// construction at each use. Fields are emitted in declaration order.
/// Returns `nullptr` when `type` isn't a struct or any field's value is
/// neither a scalar nor such a struct (a string, a list, ...).
auto checker::build_static_struct_expr(const comptime::value &value,
                                       source_span span, type_id type)
    -> ast::ptr<ast::expr> {
  if (value.kind != comptime::value_kind::struct_instance) {
    return build_const_literal(value, span, type);
  }
  const auto &entry = session_.types_.entry(type);
  const auto *decl_fields = struct_fields_of(entry);
  if (decl_fields == nullptr || decl_fields->size() != value.fields.size()) {
    return nullptr;
  }
  auto literal = ast::make<ast::struct_expr>();
  literal->span = span;
  for (const auto &field : *decl_fields) {
    const auto found = value.fields.find(field.name);
    const auto field_type = struct_field_type(entry, field.name);
    if (found == value.fields.end() || !field_type.has_value()) {
      return nullptr;
    }
    auto element = build_static_struct_expr(found->second, span, *field_type);
    if (element == nullptr) {
      return nullptr;
    }
    literal->fields.push_back(ast::struct_field_init{
        .span = span, .name = field.name, .value = std::move(element)});
  }
  record_expr_type(*literal, type);
  return literal;
}

/// Attempts to fold a call to a comptime-only `static def` instance
/// (`session_.comptime_only_functions_`) reached from ordinary, non-comptime
/// code — see todo item 8. `hir::lower_module` never lowers a comptime-only
/// instance to a run-time function, so an ordinary `hir_call` naming it
/// would fail to resolve at the bytecode-compile stage. This runs the call
/// at compile time instead, through `decl` (the template) and the call's
/// type arguments, and turns the result into a literal for lowering to
/// splice in instead of a call. Returns `nullptr` (leaving the call to fall
/// back to the pre-existing, unresolvable-function behavior) when the call's
/// arguments aren't themselves compile-time constant, or the result isn't a
/// scalar `materialize_const_literal` can represent — a struct/list/variant
/// result has no route to a literal today.
auto checker::try_fold_comptime_only_call(const ast::call_expr &call,
                                          const ast::func_decl &decl,
                                          type_id result_type,
                                          const type_scope &type_slots)
    -> const ast::literal_expr * {
  // The call runs the instance its type arguments name.
  auto type_args = type_param_comptime_values(decl.type_params, type_slots);
  const auto result =
      comptime_eval_.try_eval_ordinary_call(decl, call, std::move(type_args));
  if (!result.has_value()) {
    return nullptr;
  }
  return materialize_const_literal(*result, call.span, result_type);
}

} // namespace cinder::semantic::detail
