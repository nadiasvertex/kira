#include "analysis.h"

#include "src/semantic/check.h"
#include "src/semantic/module_index.h"
#include "src/semantic/resolution.h"

namespace kira::semantic {

auto validate_semantics(const std::vector<parsed_module> &inputs,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  validate_semantics(inputs, diag, file_has_errors, semantic_options{});
}

auto validate_semantics(const std::vector<parsed_module> &inputs,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors,
                        const semantic_options &options) -> void {
  const auto session_index = build_module_session_index(inputs);
  const auto semantic_index = build_semantic_resolution_index(inputs);

  detect_duplicate_module_paths(inputs, diag, file_has_errors);
  validate_module_boundaries(session_index, diag, file_has_errors);
  validate_session_imports(inputs, session_index, diag, file_has_errors);
  validate_declaration_scopes(inputs, diag, file_has_errors);
  validate_qualified_paths(inputs, session_index, semantic_index, diag,
                           file_has_errors);

  if (options.check_names_and_types) {
    check_program(inputs, diag, file_has_errors);
  }
}

} // namespace kira::semantic
