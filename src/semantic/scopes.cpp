#include "scopes.h"

namespace kira::semantic {

/// Maps each `semantic_scope_kind` to its stable display label.
auto semantic_scope_kind_name(semantic_scope_kind kind) -> std::string_view {
  switch (kind) {
  case semantic_scope_kind::module_scope:
    return "module scope";
  case semantic_scope_kind::trait_scope:
    return "trait scope";
  case semantic_scope_kind::impl_scope:
    return "impl scope";
  case semantic_scope_kind::type_scope:
    return "type scope";
  case semantic_scope_kind::concept_scope:
    return "concept scope";
  case semantic_scope_kind::function_signature_scope:
    return "function signature scope";
  case semantic_scope_kind::function_body_scope:
    return "function body scope";
  case semantic_scope_kind::lambda_signature_scope:
    return "lambda signature scope";
  case semantic_scope_kind::lambda_body_scope:
    return "lambda body scope";
  case semantic_scope_kind::block_scope:
    return "block scope";
  case semantic_scope_kind::branch_scope:
    return "branch scope";
  case semantic_scope_kind::loop_scope:
    return "loop scope";
  case semantic_scope_kind::match_arm_scope:
    return "match arm scope";
  case semantic_scope_kind::where_scope:
    return "where scope";
  }
  return "scope";
}

} // namespace kira::semantic
