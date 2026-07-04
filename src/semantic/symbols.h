#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/k-parser/ast.h"
#include "src/k-parser/source_location.h"
#include "src/semantic/ids.h"

namespace kira::semantic {

enum class symbol_namespace : uint8_t {
  module_type_namespace,
  value_namespace,
  type_parameter_namespace,
  associated_type_namespace,
};

enum class semantic_symbol_kind : uint8_t {
  type_symbol,
  trait_symbol,
  concept_symbol,
  submodule_symbol,
  function_symbol,
  static_binding_symbol,
  parameter_symbol,
  local_binding_symbol,
  mutable_local_symbol,
  pattern_binding_symbol,
  where_binding_symbol,
  type_parameter_symbol,
  associated_type_symbol,
};

struct semantic_symbol {
  symbol_id id = k_invalid_symbol_id;
  std::string name;
  semantic_symbol_kind kind = semantic_symbol_kind::local_binding_symbol;
  symbol_namespace name_space = symbol_namespace::value_namespace;
  std::string kind_name;
  ast::visibility visibility = ast::visibility::def;
  source_location location;
  scope_id defining_scope = k_invalid_scope_id;
};

struct semantic_symbol_spec {
  std::string name;
  semantic_symbol_kind kind = semantic_symbol_kind::local_binding_symbol;
  symbol_namespace name_space = symbol_namespace::value_namespace;
  ast::visibility visibility = ast::visibility::def;
  source_location location;
};

auto semantic_symbol_kind_name(semantic_symbol_kind kind) -> std::string_view;
auto participates_in_duplicate_module_scope_check(semantic_symbol_kind kind)
    -> bool;
auto can_resolve_named_type(semantic_symbol_kind kind) -> bool;
auto can_resolve_module_reference(semantic_symbol_kind kind) -> bool;
auto symbol_namespace_name(symbol_namespace name_space) -> std::string_view;
auto module_symbol_spec(const ast::node &node, file_id_type file_id)
    -> std::optional<semantic_symbol_spec>;

} // namespace kira::semantic
