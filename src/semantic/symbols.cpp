#include "symbols.h"

#include <algorithm>
#include <utility>

namespace kira::semantic {

/// Classic dynamic-programming Levenshtein with two rolling rows; both
/// callers only care about small distances so no early-exit bound is needed.
auto edit_distance(std::string_view a, std::string_view b) -> size_t {
  const auto rows = a.size() + 1;
  const auto cols = b.size() + 1;
  auto previous = std::vector<size_t>(cols);
  auto current = std::vector<size_t>(cols);
  for (size_t j = 0; j < cols; ++j) {
    previous[j] = j;
  }
  for (size_t i = 1; i < rows; ++i) {
    current[0] = i;
    for (size_t j = 1; j < cols; ++j) {
      const auto substitution =
          previous[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      current[j] =
          std::min({previous[j] + 1, current[j - 1] + 1, substitution});
    }
    std::swap(previous, current);
  }
  return previous[cols - 1];
}

/// Keeps the candidate with the smallest edit distance below the bound,
/// skipping exact matches (the caller already knows the name didn't resolve).
auto best_suggestion(std::string_view name,
                     const std::vector<std::string> &candidates)
    -> std::optional<std::string> {
  auto best = std::optional<std::string>{};
  auto best_distance = size_t{3};
  for (const auto &candidate : candidates) {
    if (candidate == name || candidate.empty()) {
      continue;
    }
    const auto distance = edit_distance(name, candidate);
    if (distance < best_distance && distance < candidate.size()) {
      best_distance = distance;
      best = candidate;
    }
  }
  return best;
}

/// Maps each `semantic_symbol_kind` to its stable display label.
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

/// Only type/trait/concept/submodule symbols compete for a unique name
/// within one module scope; values and bindings may shadow them freely.
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

/// A named-type reference may land on a type, trait, concept, or submodule;
/// nothing else is a valid type-position target.
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

/// A module-qualified value reference may land on any module-scope
/// declaration except a local binding, parameter, or type parameter.
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

/// Maps each `symbol_namespace` to its stable display label.
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

/// Classifies one top-level or module-scope AST item into the symbol spec
/// that should be added to its enclosing module scope, or `nullopt` for
/// items that do not introduce a module-scope symbol (e.g. `use`, non-binding
/// `static` forms).
auto module_symbol_spec(const ast::node &node, file_id_type file_id)
    -> std::optional<semantic_symbol_spec> {
  switch (node.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = dynamic_cast<const ast::type_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::type_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
                .file_id = file_id,
                .span = decl.span,
            },
    };
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::trait_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
                .file_id = file_id,
                .span = decl.span,
            },
    };
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = dynamic_cast<const ast::concept_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::concept_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
                .file_id = file_id,
                .span = decl.span,
            },
    };
  }
  case ast::node_kind::sub_module_decl: {
    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::submodule_symbol,
        .name_space = symbol_namespace::module_type_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
                .file_id = file_id,
                .span = decl.span,
            },
    };
  }
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(node);
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::function_symbol,
        .name_space = symbol_namespace::value_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
                .file_id = file_id,
                .span = decl.span,
            },
    };
  }
  case ast::node_kind::static_decl: {
    const auto &decl = dynamic_cast<const ast::static_decl &>(node);
    if (decl.decl_kind != ast::static_decl_kind::binding || decl.name.empty()) {
      return std::nullopt;
    }
    return semantic_symbol_spec{
        .name = decl.name,
        .kind = semantic_symbol_kind::static_binding_symbol,
        .name_space = symbol_namespace::value_namespace,
        .visibility = decl.visibility,
        .location =
            source_location{
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
