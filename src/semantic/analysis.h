#pragma once

#include <vector>

#include "src/k-parser/ast.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/source_location.h"

namespace kira::semantic {

/// One parsed module file borrowed from the driver for semantic validation.
struct parsed_module {
  file_id_type file_id = 0;
  const ast::file *ast_file = nullptr;
};

/// Run the current phase-3 semantic validation pipeline over parsed modules.
///
/// The caller owns the ASTs and source manager. This pass only reads the ASTs,
/// emits diagnostics, and marks files as failing in `file_has_errors` so later
/// driver phases can skip metadata emission.
auto validate_semantics(const std::vector<parsed_module> &inputs,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void;

} // namespace kira::semantic
