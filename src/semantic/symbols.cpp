#include "symbols.h"

namespace kira::semantic {

auto semantic_symbol_kind_name(semantic_symbol_kind kind) -> std::string_view {
  switch (kind) {
  case semantic_symbol_kind::type_symbol:
    return "type";
  case semantic_symbol_kind::trait_symbol:
    return "trait";
  case semantic_symbol_kind::concept_symbol:
    return "concept";
  case semantic_symbol_kind::submodule_symbol:
    return "submodule";
  case semantic_symbol_kind::function_symbol:
    return "function";
  case semantic_symbol_kind::static_binding_symbol:
    return "static binding";
  case semantic_symbol_kind::parameter_symbol:
    return "parameter";
  case semantic_symbol_kind::local_binding_symbol:
    return "binding";
  case semantic_symbol_kind::mutable_local_symbol:
    return "mutable binding";
  case semantic_symbol_kind::pattern_binding_symbol:
    return "pattern binding";
  case semantic_symbol_kind::where_binding_symbol:
    return "where binding";
  case semantic_symbol_kind::type_parameter_symbol:
    return "type parameter";
  case semantic_symbol_kind::associated_type_symbol:
    return "associated type";
  }
  return "symbol";
}

auto participates_in_duplicate_module_scope_check(semantic_symbol_kind kind)
    -> bool {
  switch (kind) {
  case semantic_symbol_kind::type_symbol:
  case semantic_symbol_kind::trait_symbol:
  case semantic_symbol_kind::concept_symbol:
  case semantic_symbol_kind::submodule_symbol:
    return true;
  case semantic_symbol_kind::function_symbol:
  case semantic_symbol_kind::static_binding_symbol:
  case semantic_symbol_kind::parameter_symbol:
  case semantic_symbol_kind::local_binding_symbol:
  case semantic_symbol_kind::mutable_local_symbol:
  case semantic_symbol_kind::pattern_binding_symbol:
  case semantic_symbol_kind::where_binding_symbol:
  case semantic_symbol_kind::type_parameter_symbol:
  case semantic_symbol_kind::associated_type_symbol:
    return false;
  }
  return false;
}

auto can_resolve_named_type(semantic_symbol_kind kind) -> bool {
  switch (kind) {
  case semantic_symbol_kind::type_symbol:
  case semantic_symbol_kind::trait_symbol:
  case semantic_symbol_kind::concept_symbol:
  case semantic_symbol_kind::submodule_symbol:
    return true;
  case semantic_symbol_kind::function_symbol:
  case semantic_symbol_kind::static_binding_symbol:
  case semantic_symbol_kind::parameter_symbol:
  case semantic_symbol_kind::local_binding_symbol:
  case semantic_symbol_kind::mutable_local_symbol:
  case semantic_symbol_kind::pattern_binding_symbol:
  case semantic_symbol_kind::where_binding_symbol:
  case semantic_symbol_kind::type_parameter_symbol:
  case semantic_symbol_kind::associated_type_symbol:
    return false;
  }
  return false;
}

auto can_resolve_module_reference(semantic_symbol_kind kind) -> bool {
  switch (kind) {
  case semantic_symbol_kind::type_symbol:
  case semantic_symbol_kind::trait_symbol:
  case semantic_symbol_kind::concept_symbol:
  case semantic_symbol_kind::submodule_symbol:
  case semantic_symbol_kind::function_symbol:
  case semantic_symbol_kind::static_binding_symbol:
    return true;
  case semantic_symbol_kind::parameter_symbol:
  case semantic_symbol_kind::local_binding_symbol:
  case semantic_symbol_kind::mutable_local_symbol:
  case semantic_symbol_kind::pattern_binding_symbol:
  case semantic_symbol_kind::where_binding_symbol:
  case semantic_symbol_kind::type_parameter_symbol:
  case semantic_symbol_kind::associated_type_symbol:
    return false;
  }
  return false;
}

auto symbol_namespace_name(symbol_namespace name_space) -> std::string_view {
  switch (name_space) {
  case symbol_namespace::module_type_namespace:
    return "module/type";
  case symbol_namespace::value_namespace:
    return "value";
  case symbol_namespace::type_parameter_namespace:
    return "type parameter";
  case symbol_namespace::associated_type_namespace:
    return "associated type";
  }
  return "symbol";
}

auto module_symbol_spec(const ast::node &node, file_id_type file_id)
    -> std::optional<semantic_symbol_spec> {
  switch (node.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = static_cast<const ast::type_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::type_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = static_cast<const ast::trait_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::trait_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = static_cast<const ast::concept_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::concept_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::sub_module_decl: {
    const auto &decl = static_cast<const ast::sub_module_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::submodule_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::func_decl: {
    const auto &decl = static_cast<const ast::func_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::function_symbol,
        .name_space = symbol_namespace::value_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::static_decl: {
    const auto &decl = static_cast<const ast::static_decl &>(node);
    if (decl.decl_kind != ast::static_decl_kind::binding || decl.name.empty()) {
      return std::nullopt;
    }
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::static_binding_symbol,
        .name_space = symbol_namespace::value_namespace,
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  default:
    return std::nullopt;
  }
}

} // namespace kira::semantic
