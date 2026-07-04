#pragma once

#include <vector>

#include "src/k-parser/diagnostic.h"
#include "src/semantic/module_index.h"

namespace kira::semantic {

auto detect_duplicate_module_paths(const std::vector<parsed_module> &inputs,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void;

auto validate_module_boundaries(const module_session_index &index,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void;

auto validate_session_imports(const std::vector<parsed_module> &inputs,
                              const module_session_index &index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

auto validate_declaration_scopes(const std::vector<parsed_module> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void;

auto validate_qualified_paths(const std::vector<parsed_module> &inputs,
                              const module_session_index &session_index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

} // namespace kira::semantic
