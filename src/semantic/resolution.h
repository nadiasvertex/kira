#pragma once

#include <vector>

#include "src/k-parser/diagnostic.h"
#include "src/semantic/module_index.h"

namespace kira::semantic {

/// Reports every distinct module path declared by more than one input file.
/// Marks all contributing files as failing so later phases skip them.
auto detect_duplicate_module_paths(const std::vector<parsed_module> &inputs,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void;

/// Validates that every module with a dotted parent (e.g. `a.b`) has a
/// corresponding submodule declaration in its parent module's file, and that
/// a module declared inline is not also given a separate external file.
auto validate_module_boundaries(const module_session_index &index,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void;

/// Validates every `use` declaration whose path root is owned by this
/// compilation session: the target module/member must exist and must be
/// visible to the importer given its declared visibility.
auto validate_session_imports(const std::vector<parsed_module> &inputs,
                              const module_session_index &index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

/// Rejects duplicate type/trait/concept/submodule names within the same
/// module scope (across every file contributing to that module), recursing
/// into nested inline submodules.
auto validate_declaration_scopes(const std::vector<parsed_module> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void;

/// Walks every parsed module's AST validating qualified type paths
/// (`pkg.mod.Thing` in type position) and qualified module-value references
/// (`pkg.mod.thing` in expression position) that touch a session-owned
/// module or the `super` keyword, reporting unresolved or wrong-kind targets.
auto validate_qualified_paths(const std::vector<parsed_module> &inputs,
                              const module_session_index &session_index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void;

} // namespace kira::semantic
