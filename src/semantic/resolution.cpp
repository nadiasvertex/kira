#include "resolution.h"

#include <cstdint>
#include <format>
#include <utility>

#include "src/parser/ast_traversal.h"

namespace cinder::semantic {
/// Whether a path rooted at `root` may be read as absolute from
/// `current_module_name` without any import: the module's own root (which
/// is how every ancestor, and everything they declare, is reached) or `std`.
/// Every other root module needs a `use` — the "Visible Modules" section of
/// `spec/specification/01-core/12-modules-and-imports.md`. Keeping the
/// visible set this small is what makes a name clash decidable from the
/// referring file alone: a module no file mentions can't collide with it.
auto root_visible_without_import(std::string_view current_module_name,
                                 std::string_view root) -> bool {
  return root == module_root_name(current_module_name) || root == "std";
}

namespace {

/// Marks `file_id` as failing, tolerating an out-of-range id.
auto mark_file_has_error(std::vector<bool> &file_has_errors,
                         file_id_type file_id) -> void {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return;
  }
  file_has_errors[file_id] = true;
}

/// Emits the "duplicate declaration name in this module" diagnostic, with a
/// note pointing at the earlier declaration.
auto emit_duplicate_module_scope_symbol(
    std::string_view module_name, const module_scope_symbol_record &current,
    const module_scope_symbol_record &previous, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void {
  auto duplicate =
      diagnostic(diagnostic_level::error,
                 std::format("duplicate declaration name `{}` in module `{}`",
                             current.name, module_name),
                 current.location.file_id);
  duplicate.with_label(
      current.location.span,
      std::format("duplicate {} declaration", current.kind_name));
  duplicate.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}` was first declared here as a {}",
                             current.name, previous.kind_name),
                 previous.location.file_id)
          .with_label(
              previous.location.span,
              std::format("previous {} declaration", previous.kind_name)));
  duplicate.with_help("Give each type, trait, concept, and submodule in a "
                      "module scope a unique "
                      "name before semantic analysis continues.");
  diag.emit(duplicate);
  mark_file_has_error(file_has_errors, current.location.file_id);
}

/// Emits the "import does not resolve in this session" diagnostic, noting
/// the nearest declared module when one exists.
auto emit_unresolved_import(const module_session_index &index,
                            const use_decl_record &import_record,
                            std::string_view imported_module_name,
                            source_span import_span, diagnostic_bag &diag,
                            std::vector<bool> &file_has_errors) -> void {
  auto unresolved = diagnostic(
      diagnostic_level::error,
      std::format("import `{}` does not resolve in this compilation session",
                  imported_module_name),
      import_record.file_id);
  unresolved.with_label(import_span, "unresolved in-session import");

  if (const auto anchor =
          find_nearest_module_anchor(index, imported_module_name)) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("session-owned module `{}` is declared here",
                               anchor->module_name),
                   anchor->location.file_id)
            .with_label(anchor->location.span, "nearest declared module"));
  }

  unresolved.with_help(
      std::format("Load module `{}` into this compilation session, or change "
                  "the import to reference an external module namespace.",
                  imported_module_name));
  diag.emit(unresolved);
  mark_file_has_error(file_has_errors, import_record.file_id);
}

/// Emits the "module is not visible from module" diagnostic when an import
/// target exists but its visibility excludes the importer.
auto emit_inaccessible_import(const submodule_declaration_record &declaration,
                              const use_decl_record &import_record,
                              std::string_view imported_module_name,
                              source_span import_span, diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  auto inaccessible = diagnostic(
      diagnostic_level::error,
      std::format("module `{}` is not visible from module `{}`",
                  imported_module_name, import_record.importer_module_name),
      import_record.file_id);
  inaccessible.with_label(import_span, "imported here");
  inaccessible.children.push_back(
      diagnostic(
          diagnostic_level::note,
          std::format("module `{}` is declared here with `{}` visibility",
                      imported_module_name,
                      visibility_name(declaration.visibility)),
          declaration.location.file_id)
          .with_label(declaration.location.span,
                      "restricted module declaration"));
  inaccessible.with_help(
      visibility_help(declaration.visibility, declaration.parent_module_name));
  diag.emit(inaccessible);
  mark_file_has_error(file_has_errors, import_record.file_id);
}

/// Validates one imported module path: it must exist in the session, must
/// not be blocked by an upstream error, and (if it is declared as a
/// submodule) must be visible to the importer. Returns whether the target
/// is usable.
auto validate_import_target(const module_session_index &index,
                            const use_decl_record &import_record,
                            std::string_view imported_module_name,
                            source_span import_span, diagnostic_bag &diag,
                            std::vector<bool> &file_has_errors) -> bool {
  if (!session_contains_module(index, imported_module_name)) {
    emit_unresolved_import(index, import_record, imported_module_name,
                           import_span, diag, file_has_errors);
    return false;
  }

  if (module_resolution_blocked_by_errors(index, imported_module_name,
                                          file_has_errors)) {
    return false;
  }

  const auto *declaration = find_submodule_declaration_by_name(
      index.submodule_declarations, imported_module_name);
  if (declaration == nullptr) {
    return true;
  }

  if (has_file_error(file_has_errors, declaration->parent_file_id)) {
    return false;
  }

  if (!is_import_visible(*declaration, import_record.importer_module_name,
                         import_record.file_id)) {
    emit_inaccessible_import(*declaration, import_record, imported_module_name,
                             import_span, diag, file_has_errors);
    return false;
  }

  return true;
}

/// Emits the "module has no member" diagnostic for a `use` whose leaf names
/// neither a submodule nor a declaration of the resolved parent module,
/// suggesting the closest declared name when one is similar.
auto emit_missing_import_member(const module_session_index &index,
                                const semantic_resolution_index &semantic_index,
                                const use_decl_record &import_record,
                                std::string_view parent_module_name,
                                std::string_view member_name,
                                source_span import_span, diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void {
  auto missing = diagnostic(diagnostic_level::error,
                            std::format("module `{}` has no member named `{}`",
                                        parent_module_name, member_name),
                            import_record.file_id);
  missing.with_label(import_span,
                     std::format("not declared in `{}`", parent_module_name));

  if (const auto anchor =
          find_nearest_module_anchor(index, parent_module_name)) {
    missing.children.push_back(
        diagnostic(
            diagnostic_level::note,
            std::format("module `{}` is declared here", anchor->module_name),
            anchor->location.file_id)
            .with_label(anchor->location.span, "imported module"));
  }

  auto candidates = std::vector<std::string>{};
  if (const auto *scope =
          find_module_scope(semantic_index, parent_module_name)) {
    for (const auto symbol : scope->symbols) {
      if (const auto *record =
              find_semantic_symbol(semantic_index.session, symbol)) {
        candidates.push_back(record->name);
      }
    }
  }
  if (const auto suggestion = best_suggestion(member_name, candidates)) {
    missing.with_help(std::format(
        "Did you mean `{}`? Only names declared in `{}` can be imported "
        "from it.",
        *suggestion, parent_module_name));
  } else {
    missing.with_help(std::format(
        "Declare `{}` in module `{}` before importing it, or import a name "
        "that module already declares.",
        member_name, parent_module_name));
  }
  diag.emit(missing);
  mark_file_has_error(file_has_errors, import_record.file_id);
}

/// Validates the leaf of a member-selecting import (`use a.b.c`,
/// `use a.b.{c}`): the leaf may name a submodule of the already-validated
/// parent module (checked like any module import, including visibility) or
/// any declaration in the parent's module scope. Returns whether the leaf
/// resolved.
auto validate_import_leaf(const module_session_index &index,
                          const semantic_resolution_index &semantic_index,
                          const use_decl_record &import_record,
                          std::string_view parent_module_name,
                          std::string_view leaf_name, source_span leaf_span,
                          diagnostic_bag &diag,
                          std::vector<bool> &file_has_errors) -> bool {
  const auto leaf_module_name =
      append_module_name(parent_module_name, leaf_name);
  if (session_contains_module(index, leaf_module_name)) {
    return validate_import_target(index, import_record, leaf_module_name,
                                  leaf_span, diag, file_has_errors);
  }
  if (find_module_scope_symbol(semantic_index, parent_module_name, leaf_name) !=
      nullptr) {
    return true;
  }
  emit_missing_import_member(index, semantic_index, import_record,
                             parent_module_name, leaf_name, leaf_span, diag,
                             file_has_errors);
  return false;
}

/// Recursively checks `items` for duplicate type/trait/concept/submodule
/// names within the same module scope, descending into inline submodules
/// under their own qualified name.
auto validate_module_scope(const std::vector<ast::ptr<ast::node>> &items,
                           std::string_view module_name, file_id_type file_id,
                           diagnostic_bag &diag,
                           std::vector<bool> &file_has_errors) -> void {
  auto seen_symbols = std::vector<module_scope_symbol_record>{};

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }

    if (const auto symbol = module_scope_symbol(*item, file_id);
        symbol && participates_in_duplicate_module_scope_check(symbol->kind)) {
      const module_scope_symbol_record *previous = nullptr;
      for (const auto &candidate : seen_symbols) {
        if (candidate.name == symbol->name) {
          previous = &candidate;
          break;
        }
      }

      if (previous != nullptr) {
        emit_duplicate_module_scope_symbol(module_name, *symbol, *previous,
                                           diag, file_has_errors);
      } else {
        seen_symbols.push_back(*symbol);
      }
    }

    if (item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = ast::cast<const ast::sub_module_decl>(*item);
    if (decl.is_functor()) {
      // Functor bodies are validated per instantiation, not as a module scope.
      continue;
    }
    if (!decl.items.empty()) {
      validate_module_scope(decl.items,
                            append_module_name(module_name, decl.name), file_id,
                            diag, file_has_errors);
    }
  }
}

/// Outcome of trying to resolve a qualified path against the session.
enum class qualified_path_status : uint8_t {
  resolved,   ///< The path names a declared module or module-scope symbol.
  unresolved, ///< No prefix of the path names a declared module.
  blocked,    ///< A prefix resolved, but its file already has a recorded error.
};

/// Result of matching a (possibly relative) qualified path against the
/// session's known modules and their symbols.
struct qualified_path_resolution {
  qualified_path_status status =
      qualified_path_status::unresolved; ///< Overall outcome.
  std::string absolute_path; ///< Absolute path text that was checked.
  std::string
      containing_module_name; ///< Longest module-path prefix that resolved.
  size_t resolved_module_prefix =
      0; ///< Segment count consumed by that module prefix.
  const module_scope_symbol_record *symbol =
      nullptr; ///< Symbol the remaining segment named, if any.
  /// Set when the path's first segment names a root module this program
  /// declares but the referring module cannot see — see
  /// `root_visible_without_import`. The diagnostic then points at the
  /// missing `use` instead of at the path's contents.
  std::string invisible_root;
};

/// Ambient state threaded through the qualified-path validation walk: the
/// fully-qualified module and file the currently-visited node belongs to.
struct semantic_walk_context {
  std::string module_name;
  file_id_type file_id = 0;
  /// The enclosing *file*'s import aliases, owned by `validate_qualified_
  /// paths` for the length of the walk. Borrowed rather than copied because
  /// an inline submodule inherits its file's imports unchanged.
  const module_alias_map *aliases = nullptr;
};

/// Only multi-segment or `super`-rooted named-type paths are qualified
/// references worth resolving here; a single unqualified segment is an
/// ordinary type name handled elsewhere.
auto should_validate_named_type_path(const ast::named_type &type) -> bool {
  if (type.path.empty()) {
    return false;
  }
  // `self.output` names an associated type on the implementing/self type,
  // not a module-qualified path; that reference is resolved and validated
  // by the semantic checker, which knows the enclosing trait/impl.
  if (type.path.front() == "self") {
    return false;
  }
  return type.path.front() == "super" || type.path.size() > 1;
}

/// Only a `super`-rooted or session-owned dotted path is a module
/// reference worth resolving here; anything else may be an external
/// namespace this session doesn't know about.
auto should_validate_module_reference(const ast::module_path_expr &expr,
                                      const module_session_index &index)
    -> bool {
  return !expr.segments.empty() &&
         (expr.segments.front() == "super" ||
          session_owns_root_module(index, expr.segments.front()));
}

/// Prefers `resolved` over `blocked` over `unresolved`, then the longer
/// matched module prefix, then a candidate that stopped at a known symbol —
/// used to pick the best of several relative/absolute interpretations of
/// one path.
auto is_better_resolution_candidate(const qualified_path_resolution &candidate,
                                    const qualified_path_resolution &current)
    -> bool {
  if (candidate.status != current.status) {
    if (candidate.status == qualified_path_status::resolved) {
      return true;
    }
    if (current.status == qualified_path_status::resolved) {
      return false;
    }
    if (candidate.status == qualified_path_status::blocked) {
      return true;
    }
    if (current.status == qualified_path_status::blocked) {
      return false;
    }
  }

  if (candidate.resolved_module_prefix != current.resolved_module_prefix) {
    return candidate.resolved_module_prefix > current.resolved_module_prefix;
  }
  return candidate.symbol != nullptr && current.symbol == nullptr;
}

/// Resolves an already-absolute segment list against the session: finds the
/// longest prefix that names a declared module, then (if segments remain)
/// looks up the next segment as a module-scope symbol.
auto resolve_absolute_qualified_path(
    const semantic_resolution_index &semantic_index,
    const module_session_index &session_index,
    const std::vector<std::string> &absolute_segments,
    const std::vector<bool> &file_has_errors) -> qualified_path_resolution {
  auto result = qualified_path_resolution{
      .status = qualified_path_status::unresolved,
      .absolute_path = join_strings(absolute_segments, "."),
      .containing_module_name = {},
      .resolved_module_prefix = 0,
      .symbol = nullptr,
  };

  if (absolute_segments.empty()) {
    result.status = qualified_path_status::resolved;
    return result;
  }

  for (size_t i = 1; i <= absolute_segments.size(); ++i) {
    const auto prefix = join_module_path_prefix(absolute_segments, i);
    if (!session_contains_module(session_index, prefix)) {
      continue;
    }

    result.resolved_module_prefix = i;
    result.containing_module_name = prefix;
  }

  if (result.resolved_module_prefix == 0) {
    return result;
  }

  if (module_resolution_blocked_by_errors(
          session_index, result.containing_module_name, file_has_errors)) {
    result.status = qualified_path_status::blocked;
    return result;
  }

  if (result.resolved_module_prefix == absolute_segments.size()) {
    result.status = qualified_path_status::resolved;
    return result;
  }

  result.symbol = find_module_scope_symbol(
      semantic_index, result.containing_module_name,
      absolute_segments[result.resolved_module_prefix]);
  if (result.symbol == nullptr) {
    return result;
  }

  if (result.resolved_module_prefix + 1 == absolute_segments.size()) {
    result.status = qualified_path_status::resolved;
  }

  return result;
}

/// Resolves a named-type path, which may start with `super` (relative to
/// the current module's parent), be implicitly relative to the current
/// module, be absolute if its root is a session-owned module, or lead with a
/// name one of the file's imports bound — trying each interpretation and
/// keeping the better one.
auto resolve_named_type_path(const semantic_resolution_index &semantic_index,
                             const module_session_index &session_index,
                             std::string_view current_module_name,
                             const std::vector<std::string> &path,
                             const std::vector<bool> &file_has_errors,
                             const module_alias_map *aliases = nullptr)
    -> qualified_path_resolution {
  if (path.empty()) {
    return qualified_path_resolution{.status = qualified_path_status::resolved};
  }

  if (path.front() == "super") {
    const auto parent_name = parent_module_name(current_module_name);
    if (parent_name.empty()) {
      return qualified_path_resolution{};
    }
    if (path.size() == 1) {
      return qualified_path_resolution{
          .status = qualified_path_status::resolved,
          .absolute_path = parent_name,
          .containing_module_name = parent_name,
          .resolved_module_prefix = split_module_name(parent_name).size(),
          .symbol = nullptr,
      };
    }

    auto absolute_segments = split_module_name(parent_name);
    absolute_segments.insert(absolute_segments.end(), path.begin() + 1,
                             path.end());
    return resolve_absolute_qualified_path(semantic_index, session_index,
                                           absolute_segments, file_has_errors);
  }

  auto best = qualified_path_resolution{};

  auto relative_segments = split_module_name(current_module_name);
  relative_segments.insert(relative_segments.end(), path.begin(), path.end());
  auto relative = resolve_absolute_qualified_path(
      semantic_index, session_index, relative_segments, file_has_errors);
  best = std::move(relative);

  const auto session_root =
      session_owns_root_module(session_index, path.front());
  const auto root_visible =
      root_visible_without_import(current_module_name, path.front());
  if (session_root && root_visible) {
    auto absolute = resolve_absolute_qualified_path(
        semantic_index, session_index, path, file_has_errors);
    if (is_better_resolution_candidate(absolute, best)) {
      best = std::move(absolute);
    }
  }

  // Through the file's own imports last, and only as an improvement: after
  // `use p.q`, the `q` in `q.holder` is the imported module, not a submodule
  // of the current one. Without this the relative reading is the only one
  // tried, and the path is reported unresolved while naming an absolute path
  // (`app.q.holder`) the user never wrote — pointing them at declaring the
  // symbol locally rather than at the import that already brought it in.
  if (aliases != nullptr) {
    if (const auto alias = aliases->find(path.front());
        alias != aliases->end()) {
      auto aliased = alias->second;
      aliased.insert(aliased.end(), path.begin() + 1, path.end());
      auto through_import = resolve_absolute_qualified_path(
          semantic_index, session_index, aliased, file_has_errors);
      if (is_better_resolution_candidate(through_import, best)) {
        best = std::move(through_import);
      }
    }
  }

  if (best.status == qualified_path_status::unresolved && session_root &&
      !root_visible &&
      (aliases == nullptr || !aliases->contains(path.front()))) {
    best.invisible_root = path.front();
  }
  return best;
}

/// Resolves a module-qualified value reference. Unlike named-type paths,
/// these are only reached here when already known to be `super`-rooted or
/// rooted at a session-owned module name (see
/// `should_validate_module_reference`), so no relative-path guessing is
/// needed. An import binding that name wins; otherwise the root must be
/// visible without one.
/// A value-position path may run past the member it names when that member
/// is a `static`: `geo.origin.y` is the module `geo`, its `static origin`,
/// then field access (spec "Dotted Names", rule 3 — the rest of the path is
/// resolved inside the module, and inside a value that means fields). The
/// checker types the projection; this only stops the path from being
/// reported as running past a declaration.
auto accept_static_field_projection(qualified_path_resolution result)
    -> qualified_path_resolution {
  if (result.status == qualified_path_status::unresolved &&
      result.symbol != nullptr &&
      result.symbol->kind == semantic_symbol_kind::static_binding_symbol) {
    result.status = qualified_path_status::resolved;
  }
  return result;
}

auto resolve_module_reference_path(
    const semantic_resolution_index &semantic_index,
    const module_session_index &session_index,
    std::string_view current_module_name, const std::vector<std::string> &path,
    const std::vector<bool> &file_has_errors, const module_alias_map *aliases)
    -> qualified_path_resolution {
  if (path.empty()) {
    return qualified_path_resolution{.status = qualified_path_status::resolved};
  }

  if (path.front() == "super") {
    const auto parent_name = parent_module_name(current_module_name);
    if (parent_name.empty()) {
      return qualified_path_resolution{};
    }

    auto absolute_segments = split_module_name(parent_name);
    absolute_segments.insert(absolute_segments.end(), path.begin() + 1,
                             path.end());
    return resolve_absolute_qualified_path(semantic_index, session_index,
                                           absolute_segments, file_has_errors);
  }

  if (aliases != nullptr) {
    if (const auto alias = aliases->find(path.front());
        alias != aliases->end()) {
      auto aliased = alias->second;
      aliased.insert(aliased.end(), path.begin() + 1, path.end());
      return resolve_absolute_qualified_path(semantic_index, session_index,
                                             aliased, file_has_errors);
    }
  }
  if (!root_visible_without_import(current_module_name, path.front())) {
    return qualified_path_resolution{.absolute_path = join_strings(path, "."),
                                     .invisible_root = path.front()};
  }
  return resolve_absolute_qualified_path(semantic_index, session_index, path,
                                         file_has_errors);
}

/// Emits the "`super` has no parent module here" diagnostic for a `super`-
/// rooted path used from a top-level (parentless) module.
auto emit_invalid_super_path(std::string_view kind_name,
                             std::string_view path_text,
                             source_location location,
                             std::string_view module_name, diagnostic_bag &diag,
                             std::vector<bool> &file_has_errors) -> void {
  auto invalid =
      diagnostic(diagnostic_level::error,
                 std::format("{} `{}` cannot be resolved from root module `{}`",
                             kind_name, path_text, module_name),
                 location.file_id);
  invalid.with_label(location.span, "`super` has no parent module here");
  invalid.with_help("Use a session-owned module path, or remove the "
                    "parent-qualified prefix.");
  diag.emit(invalid);
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Emits the "path does not resolve from module" diagnostic, adding a note
/// pointing at whichever declaration the path search got closest to (the
/// symbol it stopped at, or the nearest resolved module).
auto emit_unresolved_qualified_path(
    std::string_view kind_name, std::string_view path_text,
    source_location location, std::string_view module_name,
    const qualified_path_resolution &resolution,
    const module_session_index &session_index, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void {
  auto unresolved =
      diagnostic(diagnostic_level::error,
                 std::format("{} `{}` does not resolve from module `{}`",
                             kind_name, path_text, module_name),
                 location.file_id);
  unresolved.with_label(location.span, std::format("unresolved {}", kind_name));

  if (!resolution.absolute_path.empty() &&
      resolution.absolute_path != path_text) {
    unresolved.children.emplace_back(
        diagnostic_level::note,
        std::format("checked absolute path `{}`", resolution.absolute_path),
        location.file_id);
  }

  if (resolution.symbol != nullptr) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("`{}` is declared here as a {}",
                               resolution.symbol->name,
                               resolution.symbol->kind_name),
                   resolution.symbol->location.file_id)
            .with_label(resolution.symbol->location.span,
                        "path stops at this declaration"));
  } else if (!resolution.containing_module_name.empty()) {
    if (const auto anchor = find_nearest_module_anchor(
            session_index, resolution.containing_module_name)) {
      unresolved.children.push_back(
          diagnostic(
              diagnostic_level::note,
              std::format("module `{}` is declared here", anchor->module_name),
              anchor->location.file_id)
              .with_label(anchor->location.span, "nearest resolved module"));
    }
  }

  if (!resolution.invisible_root.empty()) {
    unresolved.with_help(std::format(
        "Module `{0}` exists, but `{1}` can't see it: from inside `{1}`, a "
        "path can start with `{2}` (this module's own root), `std`, or a "
        "name brought in by `use`. Add `use {0}` (or `use` the specific "
        "module you need) to this file.",
        resolution.invisible_root, module_name, module_root_name(module_name)));
  } else {
    unresolved.with_help(
        "Declare the referenced symbol in that module, or change the path to "
        "one that exists in this compilation session.");
  }
  diag.emit(unresolved);
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Emits the "resolves to a X, not a type" diagnostic when a qualified type
/// path fully resolves but to a symbol kind that cannot be used as a type
/// (e.g. a function). A no-op if the resolution has no symbol to blame.
auto emit_non_type_qualified_path(std::string_view path_text,
                                  source_location location,
                                  const qualified_path_resolution &resolution,
                                  diagnostic_bag &diag,
                                  std::vector<bool> &file_has_errors) -> void {
  if (resolution.symbol == nullptr) {
    return;
  }

  auto wrong_kind = diagnostic(
      diagnostic_level::error,
      std::format("qualified type path `{}` resolves to a {}, not a type",
                  path_text, resolution.symbol->kind_name),
      location.file_id);
  wrong_kind.with_label(location.span, "used here as a type");
  wrong_kind.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}` is declared here as a {}",
                             resolution.symbol->name,
                             resolution.symbol->kind_name),
                 resolution.symbol->location.file_id)
          .with_label(
              resolution.symbol->location.span,
              std::format("{} declaration", resolution.symbol->kind_name)));
  wrong_kind.with_help("Type positions currently accept types, traits, "
                       "concepts, and modules only.");
  diag.emit(wrong_kind);
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Resolves the qualified path of a named type (when it qualifies, see
/// `should_validate_named_type_path`) and reports an invalid `super`, an
/// unresolved path, or a target that is not a type.
auto validate_named_type_path(const ast::named_type &named,
                              const semantic_walk_context &context,
                              const semantic_resolution_index &semantic_index,
                              const module_session_index &session_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  if (!should_validate_named_type_path(named) || named.path.empty()) {
    return;
  }

  const auto path_text = join_strings(named.path, ".");
  const auto location = source_location{
      .file_id = context.file_id,
      .span = named.span,
  };

  if (named.path.front() == "super" &&
      parent_module_name(context.module_name).empty()) {
    emit_invalid_super_path("qualified type path", path_text, location,
                            context.module_name, diag, file_has_errors);
    return;
  }

  const auto resolution = resolve_named_type_path(
      semantic_index, session_index, context.module_name, named.path,
      file_has_errors, context.aliases);
  if (resolution.status == qualified_path_status::blocked) {
    return;
  }
  if (resolution.status != qualified_path_status::resolved) {
    emit_unresolved_qualified_path("qualified type path", path_text, location,
                                   context.module_name, resolution,
                                   session_index, diag, file_has_errors);
    return;
  }
  if (resolution.symbol != nullptr &&
      !can_resolve_named_type(resolution.symbol->kind)) {
    emit_non_type_qualified_path(path_text, location, resolution, diag,
                                 file_has_errors);
  }
  return;
}

/// Walks the tree under `node` looking for qualified type paths worth
/// checking. Only the kinds with their own rule are handled here; every
/// other node's children come from `ast::for_each_child`.
auto validate_ast_node(const ast::node &node,
                       const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> void;

/// Validates each item in `items` in order, skipping null entries.
auto validate_node_list(const std::vector<ast::ptr<ast::node>> &items,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  for (const auto &item : items) {
    if (item != nullptr) {
      validate_ast_node(*item, context, semantic_index, session_index, diag,
                        file_has_errors);
    }
  }
}

auto validate_ast_node(const ast::node &node,
                       const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> void {
  if (node.has_error) {
    return;
  }

  const auto recurse = [&](const ast::node &child) -> void {
    validate_ast_node(child, context, semantic_index, session_index, diag,
                      file_has_errors);
  };

  switch (node.kind) {
  case ast::node_kind::named_type: {
    // Type arguments first, then the path itself.
    ast::for_each_child(node, recurse);
    const auto *named = ast::as<ast::named_type>(&node);
    if (named != nullptr) {
      validate_named_type_path(*named, context, semantic_index, session_index,
                               diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::use_decl:
  case ast::node_kind::quote_expr:
  case ast::node_kind::quote_type:
    // Imports are validated by the import pass, and quoted code is not
    // resolved until it is spliced.
  case ast::node_kind::module_path_expr:
    // Deliberately not validated here: `a.b` is field access when `a` is a
    // binding in scope and a module reference otherwise, and only the
    // checker knows every binding in scope. It classifies the path and calls
    // `validate_module_reference` for the module-rooted ones — deciding
    // here instead, with no local scopes, is how a user `module s` once
    // broke every `for s in ...: s.field` in the standard library.
    // A struct-literal head (`q.holder { ... }`) is always a type path and
    // is resolved by the checker's `find_type_decl_by_path`.
    return;

  case ast::node_kind::sub_module_decl: {
    const auto *decl = ast::as<ast::sub_module_decl>(&node);
    if (decl == nullptr || decl->is_functor()) {
      // A functor's body references its module parameters (`DB.conn`), which
      // only resolve once instantiated. It is name-resolved and type-checked
      // per instantiation, so it is skipped here (see
      // `spec/module-values-design.md` §4).
      return;
    }
    auto child_context = semantic_walk_context{
        .module_name = append_module_name(context.module_name, decl->name),
        .file_id = context.file_id,
        // An inline submodule sits in the same file, and sees the same `use`
        // declarations its enclosing file wrote.
        .aliases = context.aliases,
    };
    validate_node_list(decl->items, child_context, semantic_index,
                       session_index, diag, file_has_errors);
    return;
  }

  default:
    ast::for_each_child(node, recurse);
    return;
  }
}

} // namespace

/// Collects `file`'s module-import aliases, so a qualified path written
/// through one (`q.holder` after `use p.q`) can be resolved the way the
/// checker already resolves it (`check.cpp`'s `resolve_named_type`).
auto collect_module_aliases(const ast::file &file) -> module_alias_map {
  auto aliases = module_alias_map{};
  for (const auto &item : file.items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::use_decl) {
      continue;
    }
    const auto &use = ast::cast<const ast::use_decl>(*item);
    // A functor instantiation binds its alias to a module the checker
    // materializes later, which is not something this pass can resolve a
    // path through; `build_import_bindings` skips these for the same reason.
    if (!use.instantiation_args.empty() || use.path.empty()) {
      continue;
    }
    if (!use.selector.has_value()) {
      aliases.emplace(use.alias.value_or(use.path.back()), use.path);
      continue;
    }
    if (use.selector->kind == ast::use_selector_kind::wildcard) {
      continue;
    }
    for (const auto &selected : use.selector->items) {
      auto full = use.path;
      full.push_back(selected.name);
      aliases.emplace(selected.alias.value_or(selected.name), std::move(full));
    }
  }
  return aliases;
}

/// For each module with a dotted parent, requires the parent's file to
/// declare it as a submodule, and rejects a submodule that is declared both
/// inline and as a separate external file.
auto validate_module_boundaries(const module_session_index &index,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void {
  for (const auto &module_file : index.module_files) {
    if (module_file.parent_module_name.empty() ||
        has_file_error(file_has_errors, module_file.file_id)) {
      continue;
    }

    const auto *parent =
        find_module_file(index.module_files, module_file.parent_module_name);
    if (parent == nullptr || has_file_error(file_has_errors, parent->file_id)) {
      continue;
    }

    const auto *declaration = find_submodule_declaration(
        index.submodule_declarations, parent->file_id, module_file.module_name);
    if (declaration == nullptr) {
      auto missing = diagnostic(
          diagnostic_level::error,
          std::format("module `{}` is not declared by parent module `{}`",
                      module_file.module_name, parent->module_name),
          module_file.file_id);
      missing.with_label(module_file.location.span,
                         "child module defined here");
      missing.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("parent module `{}` is declared here",
                                 parent->module_name),
                     parent->file_id)
              .with_label(parent->location.span, "parent module declaration"));
      missing.with_help(std::format(
          "Add `module {}` to `{}`, or stop compiling `{}` as a separate "
          "module.",
          module_file.child_name, parent->module_name,
          module_file.module_name));
      diag.emit(missing);

      mark_file_has_error(file_has_errors, module_file.file_id);
      mark_file_has_error(file_has_errors, parent->file_id);
      continue;
    }

    if (!declaration->is_inline) {
      continue;
    }

    auto conflict = diagnostic(
        diagnostic_level::error,
        std::format(
            "module `{}` is declared inline and cannot also be defined in a "
            "separate file",
            module_file.module_name),
        module_file.file_id);
    conflict.with_label(module_file.location.span,
                        "separate file module declaration");
    conflict.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("inline submodule `{}` is declared here",
                               module_file.child_name),
                   declaration->location.file_id)
            .with_label(declaration->location.span,
                        "inline submodule declaration"));
    conflict.with_help(std::format(
        "Keep `module {}:` inline in `{}` or move it to its own file, but "
        "not both.",
        module_file.child_name, parent->module_name));
    diag.emit(conflict);

    mark_file_has_error(file_has_errors, module_file.file_id);
    mark_file_has_error(file_has_errors, declaration->location.file_id);
  }
}

/// Collects every `use` declaration rooted in a session-owned module and
/// validates each imported target: a whole module path via
/// `validate_import_target`, or a selected/trailing member via
/// `validate_import_leaf` (which also accepts declarations of the parent
/// module, not just submodules).
auto validate_session_imports(const std::vector<parsed_module> &inputs,
                              const module_session_index &index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  const auto imports = collect_use_decl_records(inputs, file_has_errors);

  for (const auto &import_record : imports) {
    if (import_record.decl == nullptr || import_record.decl->has_error ||
        import_record.decl->path.empty() ||
        has_file_error(file_has_errors, import_record.file_id)) {
      continue;
    }

    const auto &decl = *import_record.decl;
    if (!decl.instantiation_args.empty()) {
      // A functor instantiation (`use audited[postgres] as db`) targets a
      // parameterized module, not a plain one — the checker resolves and
      // materializes it (`check_functor_instantiation`). It is not a
      // module-graph import, so the in-session import validation skips it.
      continue;
    }
    if (!session_owns_root_module(index, decl.path.front())) {
      continue;
    }

    if (!decl.selector.has_value()) {
      // `use a.b.c` — the whole path may name a module, or the trailing
      // segment may name a member of module `a.b`.
      const auto imported_module_name = join_strings(decl.path, ".");
      if (decl.path.size() == 1 ||
          session_contains_module(index, imported_module_name)) {
        validate_import_target(index, import_record, imported_module_name,
                               decl.span, diag, file_has_errors);
        continue;
      }

      auto parent_path = decl.path;
      parent_path.pop_back();
      const auto parent_module_name = join_strings(parent_path, ".");
      if (!session_contains_module(index, parent_module_name)) {
        // Neither the full path nor its parent names a module — report the
        // full path so the diagnostic matches what the user wrote.
        emit_unresolved_import(index, import_record, imported_module_name,
                               decl.span, diag, file_has_errors);
        continue;
      }
      if (validate_import_target(index, import_record, parent_module_name,
                                 decl.span, diag, file_has_errors)) {
        validate_import_leaf(index, semantic_index, import_record,
                             parent_module_name, decl.path.back(), decl.span,
                             diag, file_has_errors);
      }
      continue;
    }

    const auto base_module_name = join_strings(decl.path, ".");
    // `use a.b as c` arrives as base `a` with the selector `{b as c}`. When
    // `a.b` is itself a module the import names *it*, whether or not `a`
    // is a module too — `a` need not be (a file may declare `a.b` with no
    // `a` anywhere), and requiring it would leave a module with no parent
    // unimportable under any alias.
    if (decl.selector->kind != ast::use_selector_kind::wildcard &&
        !session_contains_module(index, base_module_name) &&
        !decl.selector->items.empty() &&
        std::ranges::all_of(
            decl.selector->items, [&](const auto &item) -> auto {
              return session_contains_module(
                  index, append_module_name(base_module_name, item.name));
            })) {
      for (const auto &item : decl.selector->items) {
        validate_import_target(index, import_record,
                               append_module_name(base_module_name, item.name),
                               item.span, diag, file_has_errors);
      }
      continue;
    }
    if (!validate_import_target(index, import_record, base_module_name,
                                decl.selector->span, diag, file_has_errors)) {
      continue;
    }

    if (decl.selector->kind == ast::use_selector_kind::wildcard) {
      continue;
    }

    for (const auto &item : decl.selector->items) {
      if (!validate_import_leaf(index, semantic_index, import_record,
                                base_module_name, item.name, item.span, diag,
                                file_has_errors)) {
        break;
      }
    }
  }
}

namespace {

/// A module name visible in some module because that module *declares* or
/// *imports* it — the names `validate_module_name_conflicts` checks every
/// other module-scope name against.
struct introduced_module_name {
  std::string module_name;  ///< Absolute path of the visible module.
  source_location location; ///< The `module`/`use` that made it visible.
  bool imported = false;    ///< Brought in by `use` rather than declared.
};

/// A non-module name in a module's scope: a declaration (`def`, `static`,
/// `type`, `trait`, `concept`, `signature`) or a name a `use` brings in
/// that is not itself a module (`use p.{f}`).
struct scope_name {
  std::string name;
  std::string kind_name;     ///< "function", "type", "import of `p.f`", ...
  source_location location;  ///< The declaration, or the `use` item.
  bool imported = false;     ///< Brought in by `use` rather than declared.
  std::string imported_path; ///< What an import names (`p.f`); else empty.
};

/// Every non-module name declared at the top of `module_name`, across every
/// file of the module. A child module is a submodule symbol here and is the
/// other side of the comparison, not a candidate.
auto module_scope_names(const semantic_resolution_index &semantic_index,
                        std::string_view module_name)
    -> std::vector<scope_name> {
  auto names = std::vector<scope_name>{};
  const auto *scope = find_module_scope(semantic_index, module_name);
  if (scope == nullptr) {
    return names;
  }
  for (const auto id : scope->symbols) {
    const auto *symbol = find_semantic_symbol(semantic_index.session, id);
    if (symbol != nullptr &&
        symbol->kind != semantic_symbol_kind::submodule_symbol) {
      names.push_back(scope_name{.name = symbol->name,
                                 .kind_name = symbol->kind_name,
                                 .location = symbol->location,
                                 .imported = false,
                                 .imported_path = {}});
    }
  }
  return names;
}

/// Reports one clash between `name` and a module visible under the same
/// name. It is reported once, where the clash was introduced: at the `use`
/// when that `use` is what brought the module in, otherwise at `name`, with
/// a note at the other side.
auto emit_module_name_conflict(const scope_name &name,
                               std::string_view module_name,
                               const introduced_module_name &visible,
                               bool report_at_module, diagnostic_bag &diag,
                               std::vector<bool> &file_has_errors) -> void {
  const auto &primary = report_at_module ? visible.location : name.location;
  auto conflict = diagnostic(
      diagnostic_level::error,
      std::format("`{}` names both {} {} and the module `{}` visible in `{}`",
                  name.name, name.imported ? "an" : "a", name.kind_name,
                  visible.module_name, module_name),
      primary.file_id);
  if (report_at_module) {
    conflict.with_label(primary.span,
                        std::format("imports module `{}` as `{}`",
                                    visible.module_name, name.name));
    conflict.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("the {} `{}` is {} here", name.kind_name,
                               name.name,
                               name.imported ? "imported" : "declared"),
                   name.location.file_id)
            .with_label(name.location.span, "same name"));
  } else {
    conflict.with_label(
        primary.span,
        std::format("this {} is named like a visible module", name.kind_name));
    conflict.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("module `{}` is {} here", visible.module_name,
                               visible.imported ? "imported" : "declared"),
                   visible.location.file_id)
            .with_label(visible.location.span, "same name"));
  }
  const auto remedy = [&]() -> std::string {
    if (name.imported) {
      return "Import one of the two under another name with `as`.";
    }
    if (!visible.imported) {
      return std::format("Rename the {} or the submodule.", name.kind_name);
    }
    return std::format("Rename the {}, or import the module under another "
                       "name: `use {} as {}_mod`.",
                       name.kind_name, visible.module_name, name.name);
  }();
  const auto other =
      name.imported ? std::format("the imported `{}`", name.imported_path)
                    : std::format("the {} `{}`", name.kind_name, name.name);
  conflict.with_help(std::format(
      "`{0}.x` could reach into the module `{1}` or into {2}, so Cinder "
      "rejects the pair rather than guess. {3}",
      name.name, visible.module_name, other, remedy));
  diag.emit(conflict);
  mark_file_has_error(file_has_errors, primary.file_id);
}

/// Reports two modules visible under one name: `imported` (a `use`, where
/// the diagnostic goes) and `other` (a declared child or an earlier `use`).
auto emit_module_module_conflict(std::string_view local_name,
                                 std::string_view module_name,
                                 const introduced_module_name &imported,
                                 const introduced_module_name &other,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void {
  auto conflict = diagnostic(
      diagnostic_level::error,
      std::format("`{}` names both the module `{}` and the module `{}` "
                  "visible in `{}`",
                  local_name, imported.module_name, other.module_name,
                  module_name),
      imported.location.file_id);
  conflict.with_label(imported.location.span,
                      std::format("imports module `{}` as `{}`",
                                  imported.module_name, local_name));
  conflict.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("module `{}` is {} here", other.module_name,
                             other.imported ? "imported" : "declared"),
                 other.location.file_id)
          .with_label(other.location.span, "same name"));
  conflict.with_help(std::format(
      "`{0}.x` could reach into either module, so Cinder rejects the pair "
      "rather than guess. Import `{1}` under another name: `use {1} as "
      "{0}_mod`.",
      local_name, imported.module_name));
  diag.emit(conflict);
  mark_file_has_error(file_has_errors, imported.location.file_id);
}

/// The names a file's `use` declarations bind, split by what they bind:
/// a module in this session, or anything else (a member of one).
struct file_imports {
  std::vector<std::pair<std::string, introduced_module_name>> modules;
  std::vector<scope_name> members;
};

/// Collects `items`' `use` bindings. Only imports rooted in this session can
/// be classified; a member import from outside it is left alone, since
/// nothing here can tell whether it names a module. A wildcard contributes
/// every name it brings in — the checker reads a wildcard-imported `def` or
/// `static` as a module-level value (`find_module_scope_value`), so it must
/// meet the same rule as a named import.
auto collect_file_imports(const std::vector<ast::ptr<ast::node>> &items,
                          file_id_type file_id,
                          const module_session_index &session_index,
                          const semantic_resolution_index &semantic_index)
    -> file_imports {
  auto imports = file_imports{};
  const auto add = [&](std::string local_name, std::string target,
                       source_span span) -> void {
    const auto location = source_location{.file_id = file_id, .span = span};
    if (session_contains_module(session_index, target)) {
      imports.modules.emplace_back(
          std::move(local_name),
          introduced_module_name{.module_name = std::move(target),
                                 .location = location,
                                 .imported = true});
      return;
    }
    const auto root = target.substr(0, target.find('.'));
    if (session_owns_root_module(session_index, root)) {
      imports.members.push_back(
          scope_name{.name = std::move(local_name),
                     .kind_name = std::format("import of `{}`", target),
                     .location = location,
                     .imported = true,
                     .imported_path = target});
    }
  };
  for (const auto &item : items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::use_decl) {
      continue;
    }
    const auto &use = ast::cast<const ast::use_decl>(*item);
    if (!use.instantiation_args.empty() || use.path.empty()) {
      continue;
    }
    if (!use.selector.has_value()) {
      add(use.alias.value_or(use.path.back()), join_strings(use.path, "."),
          use.span);
      continue;
    }
    if (use.selector->kind == ast::use_selector_kind::wildcard) {
      const auto base = join_strings(use.path, ".");
      for (auto &name : module_scope_names(semantic_index, base)) {
        imports.members.push_back(scope_name{
            .name = name.name,
            .kind_name = std::format("import of `{}.{}` (through `{}.*`)", base,
                                     name.name, base),
            .location =
                source_location{.file_id = file_id, .span = use.selector->span},
            .imported = true,
            .imported_path = append_module_name(base, name.name)});
      }
      continue;
    }
    for (const auto &selected : use.selector->items) {
      add(selected.alias.value_or(selected.name),
          append_module_name(join_strings(use.path, "."), selected.name),
          selected.span);
    }
  }
  return imports;
}

/// Reports a module import and another import (of a module or a member)
/// that bind the same local name in one file, at the later of the two.
auto check_import_import_conflicts(const file_imports &imports,
                                   std::string_view module_name,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void {
  const auto &modules = imports.modules;
  for (size_t i = 0; i < modules.size(); ++i) {
    for (size_t j = i + 1; j < modules.size(); ++j) {
      if (modules[i].first == modules[j].first &&
          modules[i].second.module_name != modules[j].second.module_name) {
        const auto later_is_j = modules[i].second.location.span.start <
                                modules[j].second.location.span.start;
        emit_module_module_conflict(
            modules[i].first, module_name,
            later_is_j ? modules[j].second : modules[i].second,
            later_is_j ? modules[i].second : modules[j].second, diag,
            file_has_errors);
      }
    }
    for (const auto &member : imports.members) {
      if (member.name == modules[i].first) {
        const auto at_module =
            member.location.span.start < modules[i].second.location.span.start;
        emit_module_name_conflict(member, module_name, modules[i].second,
                                  at_module, diag, file_has_errors);
      }
    }
  }
}

/// Checks `module_name` (a file's module, or an inline submodule inside it)
/// against the file's imports, recursing into inline submodules, which
/// share their file's imports: an imported module may not share its local
/// name with a declaration of the module, and an imported member may not
/// share its local name with a declared child module.
auto check_import_conflicts(const std::vector<ast::ptr<ast::node>> &items,
                            std::string_view module_name,
                            const file_imports &imports,
                            const module_session_index &session_index,
                            const semantic_resolution_index &semantic_index,
                            diagnostic_bag &diag,
                            std::vector<bool> &file_has_errors) -> void {
  for (const auto &name : module_scope_names(semantic_index, module_name)) {
    for (const auto &[local_name, visible] : imports.modules) {
      if (local_name == name.name) {
        emit_module_name_conflict(name, module_name, visible,
                                  /*report_at_module=*/true, diag,
                                  file_has_errors);
      }
    }
  }
  for (const auto &child : session_index.submodule_declarations) {
    if (child.parent_module_name != module_name) {
      continue;
    }
    for (const auto &[local_name, visible] : imports.modules) {
      if (local_name == child.child_name) {
        emit_module_module_conflict(
            local_name, module_name, visible,
            introduced_module_name{.module_name = child.module_name,
                                   .location = child.location,
                                   .imported = false},
            diag, file_has_errors);
      }
    }
    for (const auto &member : imports.members) {
      if (member.name == child.child_name) {
        emit_module_name_conflict(
            member, module_name,
            introduced_module_name{.module_name = child.module_name,
                                   .location = child.location,
                                   .imported = false},
            /*report_at_module=*/false, diag, file_has_errors);
      }
    }
  }
  for (const auto &item : items) {
    if (item == nullptr || item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }
    const auto &sub = ast::cast<const ast::sub_module_decl>(*item);
    if (!sub.is_functor() && !sub.items.empty()) {
      check_import_conflicts(
          sub.items, append_module_name(module_name, sub.name), imports,
          session_index, semantic_index, diag, file_has_errors);
    }
  }
}

} // namespace

/// Two passes, so each clash is reported exactly once and where it was
/// introduced: a clash with a *declared* child is a property of the module
/// (reported at the declaration), and a clash involving an import is a
/// property of the importing file (reported at the `use`). A file already
/// marked failing is skipped, which is also what keeps a same-file
/// `type c` + `module c` pair — already a duplicate declaration — from being
/// reported twice.
auto validate_module_name_conflicts(
    const std::vector<parsed_module> &inputs,
    const module_session_index &session_index,
    const semantic_resolution_index &semantic_index, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void {
  for (const auto &child : session_index.submodule_declarations) {
    for (const auto &name :
         module_scope_names(semantic_index, child.parent_module_name)) {
      if (name.name == child.child_name &&
          !has_file_error(file_has_errors, name.location.file_id) &&
          !has_file_error(file_has_errors, child.location.file_id)) {
        emit_module_name_conflict(
            name, child.parent_module_name,
            introduced_module_name{.module_name = child.module_name,
                                   .location = child.location,
                                   .imported = false},
            /*report_at_module=*/false, diag, file_has_errors);
      }
    }
  }

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }
    const auto imports = collect_file_imports(
        input.ast_file->items, input.file_id, session_index, semantic_index);
    if (imports.modules.empty() && imports.members.empty()) {
      continue;
    }
    const auto module_name =
        join_strings(input.ast_file->module_decl->path, ".");
    check_import_import_conflicts(imports, module_name, diag, file_has_errors);
    check_import_conflicts(input.ast_file->items, module_name, imports,
                           session_index, semantic_index, diag,
                           file_has_errors);
  }
}

/// Runs `validate_module_scope` over each input file's top-level items.
auto validate_declaration_scopes(const std::vector<parsed_module> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void {
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }

    validate_module_scope(input.ast_file->items,
                          join_strings(input.ast_file->module_decl->path, "."),
                          input.file_id, diag, file_has_errors);
  }
}

/// Only paths touching a session-owned module (or `super`) are checked —
/// see `should_validate_module_reference`; a path rooted anywhere else is
/// left to the checker's own lookup, exactly as before the checker became
/// the caller.
auto validate_module_reference(const ast::module_path_expr &path,
                               std::string_view module_name,
                               file_id_type file_id,
                               const module_session_index &session_index,
                               const semantic_resolution_index &semantic_index,
                               const module_alias_map *aliases,
                               diagnostic_bag &diag,
                               std::vector<bool> &file_has_errors) -> bool {
  if (!should_validate_module_reference(path, session_index) ||
      path.segments.empty()) {
    return true;
  }

  const auto path_text = join_strings(path.segments, ".");
  const auto location = source_location{
      .file_id = file_id,
      .span = path.span,
  };

  if (path.segments.front() == "super" &&
      parent_module_name(module_name).empty()) {
    emit_invalid_super_path("module-qualified reference", path_text, location,
                            module_name, diag, file_has_errors);
    return false;
  }

  const auto resolution = accept_static_field_projection(
      resolve_module_reference_path(semantic_index, session_index, module_name,
                                    path.segments, file_has_errors, aliases));
  if (resolution.status == qualified_path_status::blocked) {
    return true;
  }
  if (resolution.status != qualified_path_status::resolved ||
      (resolution.symbol != nullptr &&
       !can_resolve_module_reference(resolution.symbol->kind))) {
    emit_unresolved_qualified_path("module-qualified reference", path_text,
                                   location, module_name, resolution,
                                   session_index, diag, file_has_errors);
    return false;
  }
  return true;
}

/// Runs `validate_node_list` over each input file's top-level items,
/// walking the whole AST to find and resolve qualified type/module paths.
auto validate_qualified_paths(const std::vector<parsed_module> &inputs,
                              const module_session_index &session_index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }

    const auto aliases = collect_module_aliases(*input.ast_file);
    auto context = semantic_walk_context{
        .module_name = join_strings(input.ast_file->module_decl->path, "."),
        .file_id = input.file_id,
        .aliases = &aliases,
    };
    validate_node_list(input.ast_file->items, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

} // namespace cinder::semantic
