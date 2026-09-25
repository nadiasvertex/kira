#include "analysis.h"

#include "src/semantic/check.h"
#include "src/semantic/module_index.h"
#include "src/semantic/resolution.h"

namespace kira::semantic {

/// Delegates to the options-taking overload with the default options (full
/// pipeline, including name resolution and type checking).
auto validate_semantics(const std::vector<parsed_module> &inputs,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> checked_types {
  return validate_semantics(inputs, diag, file_has_errors, semantic_options{});
}

/// Runs the module-graph and declaration-scope validation passes (module
/// boundaries, imports, declaration scopes, qualified paths), then — unless
/// `options.check_names_and_types` is false — runs the full name-resolution
/// and type-checking pass. Multiple files may declare the same `module`
/// path — `build_semantic_session` merges their top-level declarations into
/// one module scope, the same way C++ lets several translation units reopen
/// one namespace — so there is deliberately no "duplicate module path"
/// check here.
auto validate_semantics(const std::vector<parsed_module> &inputs,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors,
                        const semantic_options &options) -> checked_types {
  const auto session_index = build_module_session_index(inputs);
  const auto semantic_index = build_semantic_resolution_index(inputs);

  validate_module_boundaries(session_index, diag, file_has_errors);
  validate_session_imports(inputs, session_index, semantic_index, diag,
                           file_has_errors);
  validate_declaration_scopes(inputs, diag, file_has_errors);
  validate_module_name_conflicts(inputs, session_index, semantic_index, diag,
                                 file_has_errors);
  validate_qualified_paths(inputs, session_index, semantic_index, diag,
                           file_has_errors);

  if (options.check_names_and_types) {
    return check_program(inputs, session_index, semantic_index, diag,
                         file_has_errors);
  }
  return checked_types{};
}

} // namespace kira::semantic
