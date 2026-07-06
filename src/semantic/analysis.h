#pragma once

#include <vector>

#include "src/k-parser/ast.h"
#include "src/k-parser/diagnostic.h"
#include "src/k-parser/source_location.h"

namespace kira::semantic {

// Forward-declared rather than including "src/semantic/types.h": that
// header already includes this one (for `parsed_module`), so including it
// back here would form a cycle. A forward declaration is enough since
// `validate_semantics` below only needs `checked_types` as a return type,
// not a complete type — callers that actually use the result already
// include types.h (transitively via check.h).
struct checked_types;

/// One parsed module file borrowed from the driver for semantic validation.
struct parsed_module {
  file_id_type file_id = 0;
  const ast::file *ast_file = nullptr;
};

/// Options controlling how much of the semantic pipeline runs.
struct semantic_options {
  /// Run name resolution and type checking after module-graph validation.
  /// Parser-focused drivers (e.g. the parser stress corpus) disable this to
  /// exercise syntax without requiring semantically complete programs.
  bool check_names_and_types = true;
};

/// Run the semantic validation pipeline over parsed modules.
///
/// The caller owns the ASTs and source manager. This pass only reads the ASTs,
/// emits diagnostics, and marks files as failing in `file_has_errors` so later
/// driver phases can skip metadata emission.
///
/// Returns the type-checking result (empty, with no node types, when
/// `options.check_names_and_types` is false) — see `checked_types` in
/// `types.h`.
[[nodiscard]] auto validate_semantics(const std::vector<parsed_module> &inputs,
                                      diagnostic_bag &diag,
                                      std::vector<bool> &file_has_errors)
    -> checked_types;

/// Overload with explicit pipeline options.
[[nodiscard]] auto validate_semantics(const std::vector<parsed_module> &inputs,
                                      diagnostic_bag &diag,
                                      std::vector<bool> &file_has_errors,
                                      const semantic_options &options)
    -> checked_types;

} // namespace kira::semantic
