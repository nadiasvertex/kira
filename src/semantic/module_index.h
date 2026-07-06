#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/session.h"

namespace kira::semantic {

/// Alias kept for call sites that predate the shared `semantic_symbol_kind`
/// naming; identical to `semantic_symbol_kind`.
using module_scope_symbol_kind = semantic_symbol_kind;
/// Alias kept for call sites that predate the shared `semantic_symbol`
/// naming; identical to `semantic_symbol`.
using module_scope_symbol_record = semantic_symbol;

/// Joins `parts` with `separator` between each (e.g. module path segments
/// joined with `.`). Returns an empty string for an empty `parts`.
auto join_strings(const std::vector<std::string> &parts,
                  std::string_view separator) -> std::string;

/// Returns the module path with its last dotted segment removed (the
/// immediate parent module), or an empty string if `module_name` has no `.`.
auto parent_module_name(std::string_view module_name) -> std::string;

/// Returns the first dotted segment of `module_name` (the top-level package).
auto module_root_name(std::string_view module_name) -> std::string_view;

/// Returns `parent` with `child` appended as a new dotted segment. Returns
/// `child` unchanged if `parent` is empty.
auto append_module_name(std::string_view parent, std::string_view child)
    -> std::string;

/// Whether `module_name` is `ancestor` itself or a dotted descendant of it.
auto is_same_or_descendant_module(std::string_view module_name,
                                  std::string_view ancestor) -> bool;

/// Returns the stable human-readable label used to describe `visibility` in
/// diagnostics (e.g. "pub", "module").
auto visibility_name(ast::visibility visibility) -> std::string_view;

/// Builds the "how to fix this" help text for a visibility-related
/// diagnostic, tailored to the declared `visibility` and the name of the
/// module (`parent_name`) that owns the restricted declaration.
auto visibility_help(ast::visibility visibility, std::string_view parent_name)
    -> std::string;

/// Returns the fully-qualified module path declared by `file`, or `nullopt`
/// if the file has no valid `module` declaration.
auto module_path_key(const ast::file &file) -> std::optional<std::string>;

/// Classifies a top-level AST item into the module-scope symbol it
/// introduces, or `nullopt` if it introduces none. Thin wrapper over
/// `module_symbol_spec` that also fills in `id`/`kind_name` for immediate use
/// as a `semantic_symbol`.
auto module_scope_symbol(const ast::node &node, file_id_type file_id)
    -> std::optional<module_scope_symbol_record>;

/// Joins the first `limit` segments of `path` with `.`, e.g. for testing
/// successively longer module-path prefixes against a session index.
auto join_module_path_prefix(const std::vector<std::string> &path, size_t limit)
    -> std::string;

/// Splits a dotted module path into its segments (the inverse of
/// `join_strings(parts, ".")`).
auto split_module_name(std::string_view module_name)
    -> std::vector<std::string>;

/// Builds a `semantic_resolution_index` (a `semantic_session` plus its
/// derived module-scope lookup tables) from every parsed module in the
/// session.
auto build_semantic_resolution_index(const std::vector<parsed_module> &inputs)
    -> semantic_resolution_index;

/// Finds the module-scope record for `module_name`, or `nullptr` if no file
/// in the session declares that exact module.
auto find_module_scope(const semantic_resolution_index &index,
                       std::string_view module_name)
    -> const module_scope_record *;

/// Finds the direct module-scope symbol named `symbol_name` within
/// `module_name`, or `nullptr` if the module or the symbol does not exist.
auto find_module_scope_symbol(const semantic_resolution_index &index,
                              std::string_view module_name,
                              std::string_view symbol_name)
    -> const semantic_symbol *;

/// Builds a `module_session_index`: every file's `module` declaration plus
/// every nested `module` item declared inside them, used to validate the
/// module graph (duplicate paths, parent/child boundaries, import targets).
auto build_module_session_index(const std::vector<parsed_module> &inputs)
    -> module_session_index;

/// Whether `file_id` has already been marked as containing an error,
/// tolerating an out-of-range id (treated as no error).
auto has_file_error(const std::vector<bool> &file_has_errors,
                    file_id_type file_id) -> bool;

/// Finds the `module_file_record` for `module_name`, or `nullptr` if no file
/// in `modules` declares it.
auto find_module_file(const std::vector<module_file_record> &modules,
                      std::string_view module_name)
    -> const module_file_record *;

/// Finds the submodule declaration named `module_name` that was declared
/// inside the file `parent_file_id`, or `nullptr` if none matches.
auto find_submodule_declaration(
    const std::vector<submodule_declaration_record> &submodules,
    file_id_type parent_file_id, std::string_view module_name)
    -> const submodule_declaration_record *;

/// Finds any submodule declaration named `module_name`, regardless of which
/// file declared it, or `nullptr` if none matches.
auto find_submodule_declaration_by_name(
    const std::vector<submodule_declaration_record> &submodules,
    std::string_view module_name) -> const submodule_declaration_record *;

/// Whether the session owns `root_name` as a top-level module root (i.e. some
/// file's module path starts with that segment).
auto session_owns_root_module(const module_session_index &index,
                              std::string_view root_name) -> bool;

/// Whether any file in the session declares exactly `module_name`.
auto session_contains_module(const module_session_index &index,
                             std::string_view module_name) -> bool;

/// Finds the closest module declaration to `module_name` in the session, for
/// use as a "nearest declared module" note on an unresolved-path diagnostic.
auto find_nearest_module_anchor(const module_session_index &index,
                                std::string_view module_name)
    -> std::optional<module_anchor_record>;

/// Whether resolving `module_name` should be suppressed because a file
/// contributing to it (or a submodule declaration naming it) already has a
/// recorded error — avoids cascading follow-on diagnostics.
auto module_resolution_blocked_by_errors(
    const module_session_index &index, std::string_view module_name,
    const std::vector<bool> &file_has_errors) -> bool;

/// Whether a submodule declared with `declaration`'s visibility is visible to
/// an importer in `importer_module_name`/`importer_file_id`, following
/// Kira's `pub`/`module`/`file`/`super` visibility rules.
auto is_import_visible(const submodule_declaration_record &declaration,
                       std::string_view importer_module_name,
                       file_id_type importer_file_id) -> bool;

/// Collects every `use` declaration across all input files (skipping files
/// already marked as failing), for session-wide import validation.
auto collect_use_decl_records(const std::vector<parsed_module> &inputs,
                              const std::vector<bool> &file_has_errors)
    -> std::vector<use_decl_record>;

} // namespace kira::semantic
