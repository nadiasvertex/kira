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

using module_scope_symbol_kind = semantic_symbol_kind;
using module_scope_symbol_record = semantic_symbol;

auto join_strings(const std::vector<std::string> &parts,
                  std::string_view separator) -> std::string;
auto parent_module_name(std::string_view module_name) -> std::string;
auto module_root_name(std::string_view module_name) -> std::string_view;
auto append_module_name(std::string_view parent,
                        std::string_view child) -> std::string;
auto is_same_or_descendant_module(std::string_view module_name,
                                  std::string_view ancestor) -> bool;
auto visibility_name(ast::visibility visibility) -> std::string_view;
auto visibility_help(ast::visibility visibility,
                     std::string_view parent_name) -> std::string;

auto module_path_key(const ast::file &file) -> std::optional<std::string>;
auto module_scope_symbol(const ast::node &node, file_id_type file_id)
    -> std::optional<module_scope_symbol_record>;
auto join_module_path_prefix(const std::vector<std::string> &path, size_t limit)
    -> std::string;
auto split_module_name(std::string_view module_name)
    -> std::vector<std::string>;

auto build_semantic_resolution_index(const std::vector<parsed_module> &inputs)
    -> semantic_resolution_index;
auto find_module_scope(const semantic_resolution_index &index,
                       std::string_view module_name)
    -> const module_scope_record *;
auto find_module_scope_symbol(const semantic_resolution_index &index,
                              std::string_view module_name,
                              std::string_view symbol_name)
    -> const semantic_symbol *;

auto build_module_session_index(const std::vector<parsed_module> &inputs)
    -> module_session_index;
auto has_file_error(const std::vector<bool> &file_has_errors,
                    file_id_type file_id) -> bool;
auto find_module_file(const std::vector<module_file_record> &modules,
                      std::string_view module_name)
    -> const module_file_record *;
auto find_submodule_declaration(
    const std::vector<submodule_declaration_record> &submodules,
    file_id_type parent_file_id, std::string_view module_name)
    -> const submodule_declaration_record *;
auto find_submodule_declaration_by_name(
    const std::vector<submodule_declaration_record> &submodules,
    std::string_view module_name) -> const submodule_declaration_record *;
auto session_owns_root_module(const module_session_index &index,
                              std::string_view root_name) -> bool;
auto session_contains_module(const module_session_index &index,
                             std::string_view module_name) -> bool;
auto find_nearest_module_anchor(const module_session_index &index,
                                std::string_view module_name)
    -> std::optional<module_anchor_record>;
auto module_resolution_blocked_by_errors(
    const module_session_index &index, std::string_view module_name,
    const std::vector<bool> &file_has_errors) -> bool;
auto is_import_visible(const submodule_declaration_record &declaration,
                       std::string_view importer_module_name,
                       file_id_type importer_file_id) -> bool;
auto collect_use_decl_records(const std::vector<parsed_module> &inputs,
                              const std::vector<bool> &file_has_errors)
    -> std::vector<use_decl_record>;

} // namespace kira::semantic
