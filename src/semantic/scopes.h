#pragma once

#include <string>
#include <vector>

#include "src/k-parser/source_location.h"
#include "src/semantic/ids.h"

namespace kira::semantic {

enum class semantic_scope_kind : uint8_t {
  module_scope,
  trait_scope,
  impl_scope,
  type_scope,
  concept_scope,
  function_signature_scope,
  function_body_scope,
  lambda_signature_scope,
  lambda_body_scope,
  block_scope,
  branch_scope,
  loop_scope,
  match_arm_scope,
  where_scope,
};

struct semantic_scope {
  scope_id id = k_invalid_scope_id;
  semantic_scope_kind kind = semantic_scope_kind::block_scope;
  scope_id parent = k_invalid_scope_id;
  file_id_type file_id = 0;
  std::string module_name;
  std::string debug_name;
  source_location location;
  std::vector<symbol_id> symbols;
};

auto semantic_scope_kind_name(semantic_scope_kind kind) -> std::string_view;

} // namespace kira::semantic
