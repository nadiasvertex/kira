#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/semantic/ids.h"
#include "src/semantic/scopes.h"
#include "src/semantic/symbols.h"

namespace kira::semantic {

struct module_file_record {
  std::string module_name;
  std::string parent_module_name;
  std::string child_name;
  source_location location;
  file_id_type file_id = 0;
  const ast::file *ast_file = nullptr;
};

struct submodule_declaration_record {
  std::string module_name;
  std::string parent_module_name;
  std::string child_name;
  source_location location;
  file_id_type parent_file_id = 0;
  ast::visibility visibility = ast::visibility::def;
  bool is_inline = false;
};

struct module_session_index {
  std::vector<module_file_record> module_files;
  std::vector<submodule_declaration_record> submodule_declarations;
};

struct module_anchor_record {
  std::string module_name;
  source_location location;
};

struct use_decl_record {
  const ast::use_decl *decl = nullptr;
  std::string importer_module_name;
  source_location location;
  file_id_type file_id = 0;
};

struct module_scope_record {
  std::string module_name;
  file_id_type file_id = 0;
  scope_id scope = k_invalid_scope_id;
  std::vector<symbol_id> symbols;
};

struct semantic_session {
  std::vector<semantic_scope> scopes;
  std::vector<semantic_symbol> symbols;
  std::vector<module_scope_record> module_scopes;
  std::unordered_map<const ast::node *, scope_id> node_scopes;
};

struct semantic_resolution_index {
  semantic_session session;
};

auto build_semantic_session(const std::vector<parsed_module> &inputs)
    -> semantic_session;
auto find_semantic_scope(const semantic_session &session, scope_id id)
    -> const semantic_scope *;
auto find_semantic_symbol(const semantic_session &session, symbol_id id)
    -> const semantic_symbol *;
auto find_node_scope(const semantic_session &session, const ast::node &node)
    -> std::optional<scope_id>;
auto resolve_symbol(const semantic_session &session, scope_id start_scope,
                    symbol_namespace name_space, std::string_view name)
    -> const semantic_symbol *;

} // namespace kira::semantic
