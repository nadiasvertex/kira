#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/ids.h"
#include "src/semantic/scopes.h"
#include "src/semantic/symbols.h"

namespace kira::semantic {

/// One file that contributes to a module, recorded for module-graph checks
/// (duplicate paths, parent/child boundary validation).
struct module_file_record {
  std::string module_name; ///< Fully-qualified module path this file declares.
  std::string parent_module_name; ///< Immediate parent module path, if any.
  std::string child_name;         ///< Last segment of `module_name`.
  source_location location; ///< Location of the file's `module` declaration.
  file_id_type file_id = 0; ///< File this record was collected from.
  const ast::file *ast_file = nullptr; ///< Borrowed AST root for this file.
};

/// One `module Name` / `module Name: ...` declaration found inside a parent
/// file, recorded so a same-named external file can be checked against it.
struct submodule_declaration_record {
  std::string module_name; ///< Fully-qualified path of the declared submodule.
  std::string parent_module_name;  ///< Path of the module declaring it.
  std::string child_name;          ///< Declared submodule name.
  source_location location;        ///< Location of the `module` declaration.
  file_id_type parent_file_id = 0; ///< File the declaration was found in.
  ast::visibility visibility = ast::visibility::def; ///< Declared visibility.
  bool is_inline = false; ///< Whether the declaration has an inline body
                          ///< (`module Name: ...`).
};

/// Session-wide index of module files and submodule declarations, used to
/// validate the module graph before deeper semantic checks run.
struct module_session_index {
  std::vector<module_file_record>
      module_files; ///< One entry per file with a `module` declaration.
  std::vector<submodule_declaration_record>
      submodule_declarations; ///< One entry per nested `module` item.
};

/// A module path plus the location where it was declared, used to point
/// diagnostics at the nearest known declaration of a module.
struct module_anchor_record {
  std::string module_name;
  source_location location;
};

/// One `use` declaration recorded for session-wide import validation.
struct use_decl_record {
  const ast::use_decl *decl = nullptr; ///< Borrowed AST node for the import.
  std::string importer_module_name;    ///< Module the import appears in.
  source_location location;            ///< Location of the import.
  file_id_type file_id = 0;            ///< File the import appears in.
};

/// Direct symbols declared at the top level of one module scope (across all
/// files that share that module path), plus the scope they were added to.
struct module_scope_record {
  std::string module_name;
  file_id_type file_id = 0;
  scope_id scope = k_invalid_scope_id;
  std::vector<symbol_id> symbols;
};

/// Owns every scope and symbol produced while walking a compilation session's
/// ASTs, plus a map from AST node to the scope active at that node — the
/// foundation later passes use to resolve names.
struct semantic_session {
  std::vector<semantic_scope> scopes; ///< All scopes, indexed by `scope_id`.
  std::vector<semantic_symbol>
      symbols; ///< All symbols, indexed by `symbol_id`.
  std::vector<module_scope_record>
      module_scopes; ///< One record per distinct module scope.
  std::unordered_map<const ast::node *, scope_id>
      node_scopes; ///< Scope active at each visited AST node.
};

/// Bundles a `semantic_session` for callers that only need the session and
/// not the surrounding module-graph bookkeeping.
struct semantic_resolution_index {
  semantic_session session;
};

/// Walks every parsed module's AST, building scopes and symbols for every
/// declaration, binding, and pattern, and recording the active scope at each
/// AST node for later lookup via `find_node_scope`/`resolve_symbol`.
auto build_semantic_session(const std::vector<parsed_module> &inputs)
    -> semantic_session;

/// Looks up a scope by id, or `nullptr` if `id` is invalid or out of range.
auto find_semantic_scope(const semantic_session &session, scope_id id)
    -> const semantic_scope *;

/// Looks up a symbol by id, or `nullptr` if `id` is invalid or out of range.
auto find_semantic_symbol(const semantic_session &session, symbol_id id)
    -> const semantic_symbol *;

/// Returns the scope that was active at `node` while building the session,
/// or `nullopt` if `node` was never visited.
auto find_node_scope(const semantic_session &session, const ast::node &node)
    -> std::optional<scope_id>;

/// Looks up `name` in `name_space`, starting at `start_scope` and walking
/// outward through parent scopes until a match is found or scopes are
/// exhausted. Returns the innermost (most recently shadowing) match.
auto resolve_symbol(const semantic_session &session, scope_id start_scope,
                    symbol_namespace name_space, std::string_view name)
    -> const semantic_symbol *;

} // namespace kira::semantic
