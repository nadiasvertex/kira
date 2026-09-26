#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/module_index.h"

namespace cinder::semantic {

/// What each name a file's `use` declarations bind locally stands for, as an
/// absolute module path: `use p.q` binds `q` to `p.q`, and `use p.q.{r as s}`
/// binds `s` to `p.q.r` for as long as `r` is itself a module.
using module_alias_map =
    std::unordered_map<std::string, std::vector<std::string>>;

/// Whether a path whose first segment is `root` may be read as an absolute
/// module path from inside `current_module_name` without an import: only
/// through the module's own root or `std` (spec "Visible Modules"). Shared
/// by path validation and the checker's own path lookups so the two can
/// never disagree about what is visible.
auto root_visible_without_import(std::string_view current_module_name,
                                 std::string_view root) -> bool;

/// Collects `file`'s module-import aliases, so a qualified path written
/// through one (`q.holder` after `use p.q`) resolves through the import.
auto collect_module_aliases(const ast::file &file) -> module_alias_map;

/// Validates that every module with a dotted parent (e.g. `a.b`) has a
/// corresponding submodule declaration in its parent module's file, and that
/// a module declared inline is not also given a separate external file.
auto validate_module_boundaries(const module_session_index &index,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void;

/// Validates every `use` declaration whose path root is owned by this
/// compilation session: the target must exist — as a module, or as a
/// declaration (type, trait, concept, function, static) of the module named
/// by the rest of the path — and must be visible to the importer given its
/// declared visibility.
auto validate_session_imports(const std::vector<parsed_module> &inputs,
                              const module_session_index &index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

/// Rejects duplicate type/trait/concept/submodule names within the same
/// module scope (across every file contributing to that module), recursing
/// into nested inline submodules.
auto validate_declaration_scopes(const std::vector<parsed_module> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void;

/// Rejects a name in a module's scope that is also the name of a module
/// *visible* there: a declaration (`def`, `static`, `type`, `trait`,
/// `concept`, `signature`) or an imported member, against a child the module
/// declares or a module the file imports. `c.x` would otherwise mean two
/// things (spec "Dotted Names", rule 2). A module that is merely *present*
/// in the program — declared somewhere, but neither declared here nor
/// imported — is no conflict, so the check never depends on names outside
/// the files involved.
auto validate_module_name_conflicts(
    const std::vector<parsed_module> &inputs,
    const module_session_index &session_index,
    const semantic_resolution_index &semantic_index, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void;

/// Walks every parsed module's AST validating qualified type paths
/// (`pkg.mod.Thing` in type position) and qualified module-value references
/// (`pkg.mod.thing` in expression position) that touch a session-owned
/// module or the `super` keyword, reporting unresolved or wrong-kind targets.
auto validate_qualified_paths(const std::vector<parsed_module> &inputs,
                              const module_session_index &session_index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

/// Validates one expression-position dotted path (`a.b.c`) that the checker
/// has classified as a module reference rather than field access on a
/// binding in scope. Called by the checker, not by `validate_qualified_
/// paths`: only the checker knows every local binding, and a path whose
/// first segment is a local is never a module path (see the "Dotted Names"
/// section of `spec/specification/01-core/12-modules-and-imports.md`).
/// `aliases` are the referring file's imports (`collect_module_aliases`).
/// Returns false after reporting a diagnostic.
auto validate_module_reference(const ast::module_path_expr &path,
                               std::string_view module_name,
                               file_id_type file_id,
                               const module_session_index &session_index,
                               const semantic_resolution_index &semantic_index,
                               const module_alias_map *aliases,
                               diagnostic_bag &diag,
                               std::vector<bool> &file_has_errors) -> bool;

} // namespace cinder::semantic
