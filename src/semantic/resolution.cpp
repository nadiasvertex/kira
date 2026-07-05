#include "resolution.h"

#include <format>
#include <utility>

namespace kira::semantic {
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
  auto duplicate = diagnostic(
      diagnostic_level::error,
      std::format("duplicate declaration name `{}` in module `{}`", current.name,
                  module_name),
      current.location.file_id);
  duplicate.with_label(current.location.span,
                       std::format("duplicate {} declaration", current.kind_name));
  duplicate.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}` was first declared here as a {}", current.name,
                             previous.kind_name),
                 previous.location.file_id)
          .with_label(previous.location.span,
                      std::format("previous {} declaration", previous.kind_name)));
  duplicate.with_help(
      "Give each type, trait, concept, and submodule in a module scope a unique "
      "name before semantic analysis continues.");
  diag.emit(std::move(duplicate));
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

  if (const auto anchor = find_nearest_module_anchor(index, imported_module_name)) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("session-owned module `{}` is declared here",
                               anchor->module_name),
                   anchor->location.file_id)
            .with_label(anchor->location.span, "nearest declared module"));
  }

  unresolved.with_help(std::format(
      "Load module `{}` into this compilation session, or change the import to reference an external module namespace.",
      imported_module_name));
  diag.emit(std::move(unresolved));
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
      diagnostic(diagnostic_level::note,
                 std::format("module `{}` is declared here with `{}` visibility",
                             imported_module_name,
                             visibility_name(declaration.visibility)),
                 declaration.location.file_id)
          .with_label(declaration.location.span, "restricted module declaration"));
  inaccessible.with_help(
      visibility_help(declaration.visibility, declaration.parent_module_name));
  diag.emit(std::move(inaccessible));
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
    emit_unresolved_import(index, import_record, imported_module_name, import_span,
                           diag, file_has_errors);
    return false;
  }

  if (module_resolution_blocked_by_errors(index, imported_module_name,
                                          file_has_errors)) {
    return false;
  }

  const auto *declaration =
      find_submodule_declaration_by_name(index.submodule_declarations,
                                         imported_module_name);
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
        emit_duplicate_module_scope_symbol(module_name, *symbol, *previous, diag,
                                           file_has_errors);
      } else {
        seen_symbols.push_back(*symbol);
      }
    }

    if (item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = static_cast<const ast::sub_module_decl &>(*item);
    if (!decl.items.empty()) {
      validate_module_scope(decl.items, append_module_name(module_name, decl.name),
                            file_id, diag, file_has_errors);
    }
  }
}

/// Outcome of trying to resolve a qualified path against the session.
enum class qualified_path_status {
  resolved,   ///< The path names a declared module or module-scope symbol.
  unresolved, ///< No prefix of the path names a declared module.
  blocked,    ///< A prefix resolved, but its file already has a recorded error.
};

/// Result of matching a (possibly relative) qualified path against the
/// session's known modules and their symbols.
struct qualified_path_resolution {
  qualified_path_status status = qualified_path_status::unresolved; ///< Overall outcome.
  std::string absolute_path;             ///< Absolute path text that was checked.
  std::string containing_module_name;    ///< Longest module-path prefix that resolved.
  size_t resolved_module_prefix = 0;     ///< Segment count consumed by that module prefix.
  const module_scope_symbol_record *symbol = nullptr; ///< Symbol the remaining segment named, if any.
};

/// Ambient state threaded through the qualified-path validation walk: the
/// fully-qualified module and file the currently-visited node belongs to.
struct semantic_walk_context {
  std::string module_name;
  file_id_type file_id = 0;
};

/// Only multi-segment or `super`-rooted named-type paths are qualified
/// references worth resolving here; a single unqualified segment is an
/// ordinary type name handled elsewhere.
auto should_validate_named_type_path(const ast::named_type &type) -> bool {
  return !type.path.empty() &&
         (type.path.front() == "super" || type.path.size() > 1);
}

/// Only a `super`-rooted or session-owned dotted path is a module
/// reference worth resolving here; anything else may be an external
/// namespace this session doesn't know about.
auto should_validate_module_reference(const ast::module_path_expr &expr,
                                      const module_session_index &index) -> bool {
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

  if (module_resolution_blocked_by_errors(session_index,
                                          result.containing_module_name,
                                          file_has_errors)) {
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
/// module, or be absolute if its root is a session-owned module — trying
/// both relative and absolute interpretations and keeping the better one.
auto resolve_named_type_path(const semantic_resolution_index &semantic_index,
                             const module_session_index &session_index,
                             std::string_view current_module_name,
                             const std::vector<std::string> &path,
                             const std::vector<bool> &file_has_errors)
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
  auto relative = resolve_absolute_qualified_path(semantic_index, session_index,
                                                  relative_segments,
                                                  file_has_errors);
  best = std::move(relative);

  if (session_owns_root_module(session_index, path.front())) {
    auto absolute = resolve_absolute_qualified_path(semantic_index, session_index,
                                                    path, file_has_errors);
    if (is_better_resolution_candidate(absolute, best)) {
      best = std::move(absolute);
    }
  }

  return best;
}

/// Resolves a module-qualified value reference. Unlike named-type paths,
/// these are only reached here when already known to be `super`-rooted or
/// session-owned absolute (see `should_validate_module_reference`), so no
/// relative-path guessing is needed.
auto resolve_module_reference_path(const semantic_resolution_index &semantic_index,
                                   const module_session_index &session_index,
                                   std::string_view current_module_name,
                                   const std::vector<std::string> &path,
                                   const std::vector<bool> &file_has_errors)
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

  return resolve_absolute_qualified_path(semantic_index, session_index, path,
                                         file_has_errors);
}

/// Emits the "`super` has no parent module here" diagnostic for a `super`-
/// rooted path used from a top-level (parentless) module.
auto emit_invalid_super_path(std::string_view kind_name, std::string_view path_text,
                             source_location location,
                             std::string_view module_name, diagnostic_bag &diag,
                             std::vector<bool> &file_has_errors) -> void {
  auto invalid = diagnostic(
      diagnostic_level::error,
      std::format("{} `{}` cannot be resolved from root module `{}`", kind_name,
                  path_text, module_name),
      location.file_id);
  invalid.with_label(location.span, "`super` has no parent module here");
  invalid.with_help(
      "Use a session-owned module path, or remove the parent-qualified prefix.");
  diag.emit(std::move(invalid));
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
  auto unresolved = diagnostic(
      diagnostic_level::error,
      std::format("{} `{}` does not resolve from module `{}`", kind_name,
                  path_text, module_name),
      location.file_id);
  unresolved.with_label(location.span, std::format("unresolved {}", kind_name));

  if (!resolution.absolute_path.empty() && resolution.absolute_path != path_text) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("checked absolute path `{}`",
                               resolution.absolute_path),
                   location.file_id));
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
    if (const auto anchor =
            find_nearest_module_anchor(session_index,
                                       resolution.containing_module_name)) {
      unresolved.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("module `{}` is declared here",
                                 anchor->module_name),
                     anchor->location.file_id)
              .with_label(anchor->location.span, "nearest resolved module"));
    }
  }

  unresolved.with_help(
      "Declare the referenced symbol in that module, or change the path to one "
      "that exists in this compilation session.");
  diag.emit(std::move(unresolved));
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
          .with_label(resolution.symbol->location.span,
                      std::format("{} declaration", resolution.symbol->kind_name)));
  wrong_kind.with_help(
      "Type positions currently accept types, traits, concepts, and modules only.");
  diag.emit(std::move(wrong_kind));
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Forward declaration: dispatches any AST node to the appropriate
/// validate_* function for its concrete kind, recursing through the whole
/// tree to find qualified type/module paths worth checking.
auto validate_ast_node(const ast::node &node, const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag,
                       std::vector<bool> &file_has_errors) -> void;

/// Forward declaration: validates a type-position expression, recursing
/// into its component types and resolving any qualified named-type path.
auto validate_type_expr(const ast::type_expr &type,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void;

/// Forward declaration: validates a value-position expression, recursing
/// into its subexpressions and resolving any qualified module-path
/// reference.
auto validate_expr(const ast::expr &expr, const semantic_walk_context &context,
                   const semantic_resolution_index &semantic_index,
                   const module_session_index &session_index,
                   diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors) -> void;

/// Forward declaration: validates a pattern, recursing into its
/// subpatterns and any embedded range-bound expressions.
auto validate_pattern(const ast::pattern &pattern,
                      const semantic_walk_context &context,
                      const semantic_resolution_index &semantic_index,
                      const module_session_index &session_index,
                      diagnostic_bag &diag,
                      std::vector<bool> &file_has_errors) -> void;

/// Forward declaration: validates each item in `items` in order via
/// `validate_ast_node`, skipping null entries.
auto validate_node_list(const std::vector<ast::ptr<ast::node>> &items,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void;

/// Validates each `+`-joined term of a trait/concept bound list.
auto validate_bound(const ast::bound &bound, const semantic_walk_context &context,
                    const semantic_resolution_index &semantic_index,
                    const module_session_index &session_index,
                    diagnostic_bag &diag,
                    std::vector<bool> &file_has_errors) -> void {
  for (const auto &term : bound.terms) {
    if (term.type != nullptr) {
      validate_type_expr(*term.type, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
  }
}

/// Validates a generic parameter's bound (for a type parameter) or value
/// type (for a value parameter), if one was written.
auto validate_type_param(const ast::type_param &param,
                         const semantic_walk_context &context,
                         const semantic_resolution_index &semantic_index,
                         const module_session_index &session_index,
                         diagnostic_bag &diag,
                         std::vector<bool> &file_has_errors) -> void {
  if (param.bound_or_type != nullptr) {
    validate_type_expr(*param.bound_or_type, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

/// Validates both sides of a `where` clause: the constrained subject and
/// its trait bound or associated-type equality target.
auto validate_where_constraint(const ast::where_constraint &constraint,
                               const semantic_walk_context &context,
                               const semantic_resolution_index &semantic_index,
                               const module_session_index &session_index,
                               diagnostic_bag &diag,
                               std::vector<bool> &file_has_errors) -> void {
  if (constraint.subject != nullptr) {
    validate_type_expr(*constraint.subject, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
  if (constraint.bound_or_type != nullptr) {
    validate_type_expr(*constraint.bound_or_type, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

/// Validates a function/lambda parameter's pattern, type annotation, and
/// default-value expression.
auto validate_param(const ast::Param &param, const semantic_walk_context &context,
                    const semantic_resolution_index &semantic_index,
                    const module_session_index &session_index,
                    diagnostic_bag &diag,
                    std::vector<bool> &file_has_errors) -> void {
  if (param.pattern != nullptr) {
    validate_pattern(*param.pattern, context, semantic_index, session_index, diag,
                     file_has_errors);
  }
  if (param.type_annotation != nullptr) {
    validate_type_expr(*param.type_annotation, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
  if (param.default_value != nullptr) {
    validate_expr(*param.default_value, context, semantic_index, session_index,
                  diag, file_has_errors);
  }
}

/// Definition: recurses into a type expression's component types (tuple
/// elements, slice/array element, ref/ptr target, fn params/return,
/// refinement base, union alternatives), and for a qualifying named-type
/// path (see `should_validate_named_type_path`), resolves it and reports an
/// invalid `super`, unresolved path, or wrong-kind target.
auto validate_type_expr(const ast::type_expr &type,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  if (type.has_error) {
    return;
  }

  switch (type.kind) {
  case ast::node_kind::named_type: {
    const auto &named = static_cast<const ast::named_type &>(type);
    for (const auto &arg : named.type_args) {
      if (arg.value != nullptr) {
        validate_ast_node(*arg.value, context, semantic_index, session_index, diag,
                          file_has_errors);
      }
    }

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
        file_has_errors);
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

  case ast::node_kind::bound_type:
    validate_bound(static_cast<const ast::bound_type &>(type).value, context,
                   semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::tuple_type: {
    const auto &tuple = static_cast<const ast::tuple_type &>(type);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_type_expr(*element, context, semantic_index, session_index, diag,
                           file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::slice_type: {
    const auto &slice = static_cast<const ast::slice_type &>(type);
    if (slice.element != nullptr) {
      validate_type_expr(*slice.element, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::array_type: {
    const auto &array = static_cast<const ast::array_type &>(type);
    if (array.element != nullptr) {
      validate_type_expr(*array.element, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (array.size != nullptr) {
      validate_ast_node(*array.size, context, semantic_index, session_index, diag,
                        file_has_errors);
    }
    return;
  }

  case ast::node_kind::ref_type: {
    const auto &ref = static_cast<const ast::ref_type &>(type);
    if (ref.inner != nullptr) {
      validate_type_expr(*ref.inner, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    return;
  }

  case ast::node_kind::ptr_type: {
    const auto &ptr = static_cast<const ast::ptr_type &>(type);
    if (ptr.inner != nullptr) {
      validate_type_expr(*ptr.inner, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    return;
  }

  case ast::node_kind::fn_type: {
    const auto &fn = static_cast<const ast::fn_type &>(type);
    for (const auto &param_type : fn.param_types) {
      if (param_type != nullptr) {
        validate_type_expr(*param_type, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    if (fn.return_type != nullptr) {
      validate_type_expr(*fn.return_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::union_type: {
    const auto &union_type = static_cast<const ast::union_type &>(type);
    for (const auto &alternative : union_type.alternatives) {
      if (alternative != nullptr) {
        validate_type_expr(*alternative, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::refinement_type: {
    const auto &refinement = static_cast<const ast::refinement_type &>(type);
    if (refinement.base != nullptr) {
      validate_type_expr(*refinement.base, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (refinement.predicate != nullptr) {
      validate_ast_node(*refinement.predicate, context, semantic_index,
                        session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::quote_type:
    return;

  default:
    return;
  }
}

/// Definition: recurses into a pattern's subpatterns (constructor args,
/// tuple/struct/array elements, option/result/ref inner patterns, or-pattern
/// alternatives, group inner) and any range-bound expressions.
auto validate_pattern(const ast::pattern &pattern,
                      const semantic_walk_context &context,
                      const semantic_resolution_index &semantic_index,
                      const module_session_index &session_index,
                      diagnostic_bag &diag,
                      std::vector<bool> &file_has_errors) -> void {
  if (pattern.has_error) {
    return;
  }

  switch (pattern.kind) {
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = static_cast<const ast::constructor_pattern &>(pattern);
    for (const auto &arg : ctor.args) {
      if (arg != nullptr) {
        validate_pattern(*arg, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::tuple_pattern: {
    const auto &tuple = static_cast<const ast::tuple_pattern &>(pattern);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_pattern(*element, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::struct_pattern: {
    const auto &struct_pattern = static_cast<const ast::struct_pattern &>(pattern);
    for (const auto &field : struct_pattern.fields) {
      if (field.pattern != nullptr) {
        validate_pattern(*field.pattern, context, semantic_index, session_index,
                         diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::array_pattern: {
    const auto &array = static_cast<const ast::array_pattern &>(pattern);
    for (const auto &element : array.elements) {
      if (element != nullptr) {
        validate_pattern(*element, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::range_pattern: {
    const auto &range = static_cast<const ast::range_pattern &>(pattern);
    if (range.start != nullptr) {
      validate_expr(*range.start, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (range.end != nullptr) {
      validate_expr(*range.end, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::option_pattern: {
    const auto &option = static_cast<const ast::option_pattern &>(pattern);
    if (option.inner != nullptr) {
      validate_pattern(*option.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::result_pattern: {
    const auto &result = static_cast<const ast::result_pattern &>(pattern);
    if (result.inner != nullptr) {
      validate_pattern(*result.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::ref_pattern: {
    const auto &ref = static_cast<const ast::ref_pattern &>(pattern);
    if (ref.inner != nullptr) {
      validate_pattern(*ref.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::or_pattern: {
    const auto &or_pattern = static_cast<const ast::or_pattern &>(pattern);
    for (const auto &alternative : or_pattern.alternatives) {
      if (alternative != nullptr) {
        validate_pattern(*alternative, context, semantic_index, session_index,
                         diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::group_pattern: {
    const auto &group = static_cast<const ast::group_pattern &>(pattern);
    if (group.inner != nullptr) {
      validate_pattern(*group.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  default:
    return;
  }
}

/// Definition: recurses into a value expression's subexpressions, and for a
/// module-path expression that qualifies (see
/// `should_validate_module_reference`), resolves it and reports an invalid
/// `super`, unresolved path, or a target that cannot be referenced this way.
auto validate_expr(const ast::expr &expr, const semantic_walk_context &context,
                   const semantic_resolution_index &semantic_index,
                   const module_session_index &session_index,
                   diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors) -> void {
  if (expr.has_error) {
    return;
  }

  switch (expr.kind) {
  case ast::node_kind::binary_expr: {
    const auto &binary = static_cast<const ast::binary_expr &>(expr);
    if (binary.lhs != nullptr) {
      validate_expr(*binary.lhs, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (binary.rhs != nullptr) {
      validate_expr(*binary.rhs, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::unary_expr: {
    const auto &unary = static_cast<const ast::unary_expr &>(expr);
    if (unary.operand != nullptr) {
      validate_expr(*unary.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::call_expr: {
    const auto &call = static_cast<const ast::call_expr &>(expr);
    if (call.callee != nullptr) {
      validate_expr(*call.callee, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        validate_expr(*arg.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::index_expr: {
    const auto &index = static_cast<const ast::index_expr &>(expr);
    if (index.object != nullptr) {
      validate_expr(*index.object, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (index.index != nullptr) {
      validate_expr(*index.index, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::field_expr: {
    const auto &field = static_cast<const ast::field_expr &>(expr);
    if (field.object != nullptr) {
      validate_expr(*field.object, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &generic_arg : field.generic_args) {
      if (generic_arg != nullptr) {
        validate_type_expr(*generic_arg, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::cast_expr: {
    const auto &cast = static_cast<const ast::cast_expr &>(expr);
    if (cast.operand != nullptr) {
      validate_expr(*cast.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (cast.target_type != nullptr) {
      validate_type_expr(*cast.target_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::try_expr: {
    const auto &try_expr = static_cast<const ast::try_expr &>(expr);
    if (try_expr.operand != nullptr) {
      validate_expr(*try_expr.operand, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::tuple_expr: {
    const auto &tuple = static_cast<const ast::tuple_expr &>(expr);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_expr(*element, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::array_expr: {
    const auto &array = static_cast<const ast::array_expr &>(expr);
    for (const auto &element : array.elements) {
      if (element != nullptr) {
        validate_expr(*element, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    if (array.fill_value != nullptr) {
      validate_expr(*array.fill_value, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (array.fill_count != nullptr) {
      validate_expr(*array.fill_count, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::struct_expr: {
    const auto &struct_expr = static_cast<const ast::struct_expr &>(expr);
    if (struct_expr.type_name != nullptr) {
      validate_expr(*struct_expr.type_name, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    for (const auto &field : struct_expr.fields) {
      if (field.value != nullptr) {
        validate_expr(*field.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::lambda_expr: {
    const auto &lambda = static_cast<const ast::lambda_expr &>(expr);
    for (const auto &param : lambda.params) {
      if (param.pattern != nullptr) {
        validate_ast_node(*param.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (param.type_annotation != nullptr) {
        validate_type_expr(*param.type_annotation, context, semantic_index,
                           session_index, diag, file_has_errors);
      }
    }
    if (lambda.return_type != nullptr) {
      validate_type_expr(*lambda.return_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (lambda.body_expr != nullptr) {
      validate_expr(*lambda.body_expr, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    validate_node_list(lambda.body_stmts, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::module_path_expr: {
    const auto &path = static_cast<const ast::module_path_expr &>(expr);
    if (!should_validate_module_reference(path, session_index) ||
        path.segments.empty()) {
      return;
    }

    const auto path_text = join_strings(path.segments, ".");
    const auto location = source_location{
        .file_id = context.file_id,
        .span = path.span,
    };

    if (path.segments.front() == "super" &&
        parent_module_name(context.module_name).empty()) {
      emit_invalid_super_path("module-qualified reference", path_text, location,
                              context.module_name, diag, file_has_errors);
      return;
    }

    const auto resolution = resolve_module_reference_path(
        semantic_index, session_index, context.module_name, path.segments,
        file_has_errors);
    if (resolution.status == qualified_path_status::blocked) {
      return;
    }
    if (resolution.status != qualified_path_status::resolved) {
      emit_unresolved_qualified_path(
          "module-qualified reference", path_text, location, context.module_name,
          resolution, session_index, diag, file_has_errors);
      return;
    }
    if (resolution.symbol != nullptr &&
        !can_resolve_module_reference(resolution.symbol->kind)) {
      emit_unresolved_qualified_path(
          "module-qualified reference", path_text, location, context.module_name,
          resolution, session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::group_expr: {
    const auto &group = static_cast<const ast::group_expr &>(expr);
    if (group.inner != nullptr) {
      validate_expr(*group.inner, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::if_expr: {
    const auto &if_expr = static_cast<const ast::if_expr &>(expr);
    for (const auto &branch : if_expr.branches) {
      if (branch.condition != nullptr) {
        validate_expr(*branch.condition, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      if (branch.let_pattern != nullptr) {
        validate_ast_node(*branch.let_pattern, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
      if (branch.let_expr != nullptr) {
        validate_expr(*branch.let_expr, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      validate_node_list(branch.body, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    validate_node_list(if_expr.else_body, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::match_expr: {
    const auto &match = static_cast<const ast::match_expr &>(expr);
    if (match.subject != nullptr) {
      validate_expr(*match.subject, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arm : match.arms) {
      if (arm.pattern != nullptr) {
        validate_ast_node(*arm.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (arm.guard != nullptr) {
        validate_expr(*arm.guard, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      if (arm.body_expr != nullptr) {
        validate_expr(*arm.body_expr, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      validate_node_list(arm.body_stmts, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::for_expr: {
    const auto &for_expr = static_cast<const ast::for_expr &>(expr);
    for (const auto &clause : for_expr.clauses) {
      for (const auto &pattern : clause.patterns) {
        if (pattern != nullptr) {
          validate_ast_node(*pattern, context, semantic_index, session_index, diag,
                            file_has_errors);
        }
      }
      if (clause.iterable != nullptr) {
        validate_expr(*clause.iterable, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
    }
    if (for_expr.guard != nullptr) {
      validate_expr(*for_expr.guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (for_expr.yield_expr != nullptr) {
      validate_expr(*for_expr.yield_expr, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::await_expr: {
    const auto &await = static_cast<const ast::await_expr &>(expr);
    if (await.operand != nullptr) {
      validate_expr(*await.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::async_expr:
    validate_node_list(static_cast<const ast::async_expr &>(expr).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::par_expr: {
    const auto &par = static_cast<const ast::par_expr &>(expr);
    for (const auto &branch : par.branches) {
      if (branch != nullptr) {
        validate_expr(*branch, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::race_expr: {
    const auto &race = static_cast<const ast::race_expr &>(expr);
    for (const auto &branch : race.branches) {
      if (branch != nullptr) {
        validate_expr(*branch, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::crew_expr:
    validate_node_list(static_cast<const ast::crew_expr &>(expr).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::on_expr: {
    const auto &on = static_cast<const ast::on_expr &>(expr);
    if (on.context_type != nullptr) {
      validate_type_expr(*on.context_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (on.sender != nullptr) {
      validate_expr(*on.sender, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(on.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::block_expr:
    validate_node_list(static_cast<const ast::block_expr &>(expr).stmts, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::splice_expr: {
    const auto &splice = static_cast<const ast::splice_expr &>(expr);
    if (splice.operand != nullptr) {
      validate_expr(*splice.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::static_expr: {
    const auto &static_expr = static_cast<const ast::static_expr &>(expr);
    if (static_expr.operand != nullptr) {
      validate_expr(*static_expr.operand, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::where_expr: {
    const auto &where = static_cast<const ast::where_expr &>(expr);
    if (where.inner != nullptr) {
      validate_expr(*where.inner, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &binding : where.bindings) {
      if (binding.value != nullptr) {
        validate_expr(*binding.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  default:
    return;
  }
}

/// Definition: the top-level dispatcher of the qualified-path validation
/// walk. Handles every declaration and statement kind directly (recursing
/// into their type annotations, bodies, and nested items), and forwards
/// type/expression/pattern node kinds to their dedicated validate_*
/// function.
auto validate_ast_node(const ast::node &node, const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag,
                       std::vector<bool> &file_has_errors) -> void {
  if (node.has_error) {
    return;
  }

  switch (node.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = static_cast<const ast::type_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.definition != nullptr) {
      validate_ast_node(*decl.definition, context, semantic_index, session_index,
                        diag, file_has_errors);
    }
    if (decl.invariant != nullptr) {
      validate_expr(*decl.invariant, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::struct_type_def: {
    const auto &def = static_cast<const ast::struct_type_def &>(node);
    for (const auto &field : def.body.fields) {
      if (field.type != nullptr) {
        validate_type_expr(*field.type, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::sum_type_def: {
    const auto &def = static_cast<const ast::sum_type_def &>(node);
    for (const auto &variant : def.body.variants) {
      for (const auto &payload : variant.payload_types) {
        if (payload != nullptr) {
          validate_type_expr(*payload, context, semantic_index, session_index,
                             diag, file_has_errors);
        }
      }
    }
    return;
  }

  case ast::node_kind::trait_decl: {
    const auto &decl = static_cast<const ast::trait_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.requires_bound.has_value()) {
      validate_bound(*decl.requires_bound, context, semantic_index, session_index,
                     diag, file_has_errors);
    }
    validate_node_list(decl.items, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::associated_type_decl_node: {
    const auto &assoc = static_cast<const ast::associated_type_decl_node &>(node);
    if (assoc.value.default_type != nullptr) {
      validate_type_expr(*assoc.value.default_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::associated_type_def_node: {
    const auto &assoc = static_cast<const ast::associated_type_def_node &>(node);
    if (assoc.value.type != nullptr) {
      validate_type_expr(*assoc.value.type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::concept_decl: {
    const auto &decl = static_cast<const ast::concept_decl &>(node);
    for (const auto &constraint : decl.constraints) {
      if (constraint.subject != nullptr) {
        validate_type_expr(*constraint.subject, context, semantic_index,
                           session_index, diag, file_has_errors);
      }
      if (constraint.bound_or_expr != nullptr) {
        validate_ast_node(*constraint.bound_or_expr, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::impl_decl: {
    const auto &decl = static_cast<const ast::impl_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.trait_type != nullptr) {
      validate_type_expr(*decl.trait_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (decl.for_type != nullptr) {
      validate_type_expr(*decl.for_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    for (const auto &constraint : decl.where_constraints) {
      validate_where_constraint(constraint, context, semantic_index,
                                session_index, diag, file_has_errors);
    }
    validate_node_list(decl.items, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::func_decl: {
    const auto &decl = static_cast<const ast::func_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.modifiers.async_context != nullptr) {
      validate_type_expr(*decl.modifiers.async_context, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    for (const auto &param : decl.params) {
      validate_param(param, context, semantic_index, session_index, diag,
                     file_has_errors);
    }
    if (decl.return_type != nullptr) {
      validate_type_expr(*decl.return_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    for (const auto &constraint : decl.where_constraints) {
      validate_where_constraint(constraint, context, semantic_index,
                                session_index, diag, file_has_errors);
    }
    for (const auto &contract : decl.contracts) {
      if (contract.condition != nullptr) {
        validate_expr(*contract.condition, context, semantic_index,
                      session_index, diag, file_has_errors);
      }
    }
    if (decl.body_expr != nullptr) {
      validate_expr(*decl.body_expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(decl.body_stmts, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::sub_module_decl: {
    const auto &decl = static_cast<const ast::sub_module_decl &>(node);
    auto child_context = semantic_walk_context{
        .module_name = append_module_name(context.module_name, decl.name),
        .file_id = context.file_id,
    };
    validate_node_list(decl.items, child_context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::static_decl: {
    const auto &decl = static_cast<const ast::static_decl &>(node);
    if (decl.type_annotation != nullptr) {
      validate_type_expr(*decl.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (decl.initializer != nullptr) {
      validate_expr(*decl.initializer, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (decl.assert_condition != nullptr) {
      validate_expr(*decl.assert_condition, context, semantic_index,
                    session_index, diag, file_has_errors);
    }
    if (decl.if_condition != nullptr) {
      validate_expr(*decl.if_condition, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    validate_node_list(decl.if_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    validate_node_list(decl.else_body, context, semantic_index, session_index,
                       diag, file_has_errors);
    for (const auto &pattern : decl.for_patterns) {
      if (pattern != nullptr) {
        validate_pattern(*pattern, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    if (decl.for_iterable != nullptr) {
      validate_expr(*decl.for_iterable, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (decl.for_guard != nullptr) {
      validate_expr(*decl.for_guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (decl.for_yield != nullptr) {
      validate_expr(*decl.for_yield, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(decl.for_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::let_stmt: {
    const auto &stmt = static_cast<const ast::let_stmt &>(node);
    if (stmt.pattern != nullptr) {
      validate_pattern(*stmt.pattern, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    if (stmt.type_annotation != nullptr) {
      validate_type_expr(*stmt.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (stmt.initializer != nullptr) {
      validate_expr(*stmt.initializer, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.else_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::var_stmt: {
    const auto &stmt = static_cast<const ast::var_stmt &>(node);
    if (stmt.type_annotation != nullptr) {
      validate_type_expr(*stmt.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (stmt.initializer != nullptr) {
      validate_expr(*stmt.initializer, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::assign_stmt: {
    const auto &stmt = static_cast<const ast::assign_stmt &>(node);
    if (stmt.target != nullptr) {
      validate_expr(*stmt.target, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.value != nullptr) {
      validate_expr(*stmt.value, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::expr_stmt: {
    const auto &stmt = static_cast<const ast::expr_stmt &>(node);
    if (stmt.expr != nullptr) {
      validate_expr(*stmt.expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::return_stmt: {
    const auto &stmt = static_cast<const ast::return_stmt &>(node);
    if (stmt.value != nullptr) {
      validate_expr(*stmt.value, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::if_stmt: {
    const auto &stmt = static_cast<const ast::if_stmt &>(node);
    for (const auto &branch : stmt.branches) {
      if (branch.condition != nullptr) {
        validate_expr(*branch.condition, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      if (branch.let_pattern != nullptr) {
        validate_ast_node(*branch.let_pattern, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
      if (branch.let_expr != nullptr) {
        validate_expr(*branch.let_expr, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      validate_node_list(branch.body, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    validate_node_list(stmt.else_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::while_stmt: {
    const auto &stmt = static_cast<const ast::while_stmt &>(node);
    if (stmt.condition != nullptr) {
      validate_expr(*stmt.condition, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.let_pattern != nullptr) {
      validate_pattern(*stmt.let_pattern, context, semantic_index, session_index,
                       diag, file_has_errors);
    }
    if (stmt.let_expr != nullptr) {
      validate_expr(*stmt.let_expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::for_stmt: {
    const auto &stmt = static_cast<const ast::for_stmt &>(node);
    for (const auto &pattern : stmt.patterns) {
      if (pattern != nullptr) {
        validate_pattern(*pattern, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    if (stmt.iterable != nullptr) {
      validate_expr(*stmt.iterable, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.guard != nullptr) {
      validate_expr(*stmt.guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::match_stmt: {
    const auto &stmt = static_cast<const ast::match_stmt &>(node);
    if (stmt.subject != nullptr) {
      validate_expr(*stmt.subject, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arm : stmt.arms) {
      if (arm.pattern != nullptr) {
        validate_ast_node(*arm.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (arm.guard != nullptr) {
        validate_expr(*arm.guard, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      if (arm.body_expr != nullptr) {
        validate_expr(*arm.body_expr, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      validate_node_list(arm.body_stmts, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::crew_stmt:
    validate_node_list(static_cast<const ast::crew_stmt &>(node).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::splice_stmt: {
    const auto &stmt = static_cast<const ast::splice_stmt &>(node);
    if (stmt.expr != nullptr) {
      validate_expr(*stmt.expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::named_type:
  case ast::node_kind::bound_type:
  case ast::node_kind::tuple_type:
  case ast::node_kind::slice_type:
  case ast::node_kind::array_type:
  case ast::node_kind::ref_type:
  case ast::node_kind::ptr_type:
  case ast::node_kind::fn_type:
  case ast::node_kind::quote_type:
  case ast::node_kind::union_type:
  case ast::node_kind::refinement_type:
    validate_type_expr(static_cast<const ast::type_expr &>(node), context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::ident_expr:
  case ast::node_kind::literal_expr:
  case ast::node_kind::binary_expr:
  case ast::node_kind::unary_expr:
  case ast::node_kind::call_expr:
  case ast::node_kind::index_expr:
  case ast::node_kind::field_expr:
  case ast::node_kind::cast_expr:
  case ast::node_kind::try_expr:
  case ast::node_kind::tuple_expr:
  case ast::node_kind::array_expr:
  case ast::node_kind::struct_expr:
  case ast::node_kind::lambda_expr:
  case ast::node_kind::match_expr:
  case ast::node_kind::if_expr:
  case ast::node_kind::for_expr:
  case ast::node_kind::await_expr:
  case ast::node_kind::async_expr:
  case ast::node_kind::par_expr:
  case ast::node_kind::race_expr:
  case ast::node_kind::crew_expr:
  case ast::node_kind::on_expr:
  case ast::node_kind::block_expr:
  case ast::node_kind::quote_expr:
  case ast::node_kind::splice_expr:
  case ast::node_kind::static_expr:
  case ast::node_kind::module_path_expr:
  case ast::node_kind::group_expr:
  case ast::node_kind::where_expr:
    validate_expr(static_cast<const ast::expr &>(node), context, semantic_index,
                  session_index, diag, file_has_errors);
    return;

  case ast::node_kind::wildcard_pattern:
  case ast::node_kind::literal_pattern:
  case ast::node_kind::binding_pattern:
  case ast::node_kind::constructor_pattern:
  case ast::node_kind::tuple_pattern:
  case ast::node_kind::struct_pattern:
  case ast::node_kind::array_pattern:
  case ast::node_kind::range_pattern:
  case ast::node_kind::option_pattern:
  case ast::node_kind::result_pattern:
  case ast::node_kind::ref_pattern:
  case ast::node_kind::or_pattern:
  case ast::node_kind::group_pattern:
    validate_pattern(static_cast<const ast::pattern &>(node), context,
                     semantic_index, session_index, diag, file_has_errors);
    return;

  default:
    return;
  }
}

/// Definition: validates each item in `items` in declaration order.
auto validate_node_list(const std::vector<ast::ptr<ast::node>> &items,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }
    validate_ast_node(*item, context, semantic_index, session_index, diag,
                      file_has_errors);
  }
}

} // namespace

/// Compares every file's declared module path against every other file's,
/// reporting the second and later declarations of the same path as errors.
auto detect_duplicate_module_paths(const std::vector<parsed_module> &inputs,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void {
  /// One module path already seen while scanning inputs, and where its
  /// first (so-far only) declaration was found.
  struct seen_module {
    std::string module_name;
    source_location location;
  };

  auto seen_modules = std::vector<seen_module>{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr) {
      continue;
    }

    auto module_name = module_path_key(*input.ast_file);
    if (!module_name.has_value()) {
      continue;
    }

    auto current_location = source_location{
        .file_id = input.file_id,
        .span = input.ast_file->module_decl->span,
    };

    const seen_module *previous = nullptr;
    for (const auto &candidate : seen_modules) {
      if (candidate.module_name == *module_name) {
        previous = &candidate;
        break;
      }
    }

    if (previous != nullptr) {
      auto duplicate = diagnostic(
          diagnostic_level::error,
          std::format("duplicate module path `{}`", *module_name),
          current_location.file_id);
      duplicate.with_label(current_location.span, "duplicate module declaration");
      duplicate.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("module `{}` was first declared here",
                                 *module_name),
                     previous->location.file_id)
              .with_label(previous->location.span,
                          "previous module declaration"));
      duplicate.with_help(
          "Give each source file a unique module path before compiling them "
          "together.");
      diag.emit(std::move(duplicate));

      mark_file_has_error(file_has_errors, current_location.file_id);
      mark_file_has_error(file_has_errors, previous->location.file_id);
      continue;
    }

    seen_modules.push_back(seen_module{
        .module_name = *module_name,
        .location = current_location,
    });
  }
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
      missing.with_label(module_file.location.span, "child module defined here");
      missing.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("parent module `{}` is declared here",
                                 parent->module_name),
                     parent->file_id)
              .with_label(parent->location.span, "parent module declaration"));
      missing.with_help(std::format(
          "Add `module {}` to `{}`, or stop compiling `{}` as a separate "
          "module.",
          module_file.child_name, parent->module_name, module_file.module_name));
      diag.emit(std::move(missing));

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
    diag.emit(std::move(conflict));

    mark_file_has_error(file_has_errors, module_file.file_id);
    mark_file_has_error(file_has_errors, declaration->location.file_id);
  }
}

/// Collects every `use` declaration rooted in a session-owned module and
/// validates each imported target (whole path, or each selected/wildcard
/// member) via `validate_import_target`.
auto validate_session_imports(const std::vector<parsed_module> &inputs,
                              const module_session_index &index,
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
    if (!session_owns_root_module(index, decl.path.front())) {
      continue;
    }

    if (!decl.selector.has_value()) {
      const auto imported_module_name = join_strings(decl.path, ".");
      validate_import_target(index, import_record, imported_module_name, decl.span,
                             diag, file_has_errors);
      continue;
    }

    const auto base_module_name = join_strings(decl.path, ".");
    if (!validate_import_target(index, import_record, base_module_name,
                                decl.selector->span, diag, file_has_errors)) {
      continue;
    }

    if (decl.selector->kind == ast::UseSelectorKind::Wildcard) {
      continue;
    }

    for (const auto &item : decl.selector->items) {
      const auto imported_module_name = append_module_name(base_module_name, item.name);
      if (!validate_import_target(index, import_record, imported_module_name,
                                  item.span, diag, file_has_errors)) {
        break;
      }
    }
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

    auto context = semantic_walk_context{
        .module_name = join_strings(input.ast_file->module_decl->path, "."),
        .file_id = input.file_id,
    };
    validate_node_list(input.ast_file->items, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

} // namespace kira::semantic
