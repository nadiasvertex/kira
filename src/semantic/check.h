#pragma once

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/module_index.h"
#include "src/semantic/types.h"

namespace kira::semantic {

/// Run name resolution and type checking over every parsed module.
///
/// This pass resolves unqualified value and type names, infers and checks
/// expression types for core language constructs, validates pattern matches
/// (including sum-type exhaustiveness), and validates trait implementations
/// (coherence, member completeness, and `requires` obligations).
///
/// Files already marked in `file_has_errors` are skipped so parse errors do
/// not cascade into low-value semantic noise.
///
/// Returns the session's `checked_types` — the interned type table and the
/// resolved type of every expression node visited — so a later pass (e.g.
/// typed lowering) can read what the checker already knows instead of
/// re-deriving it. The types remain meaningful only as long as the AST they
/// were resolved against is alive.
[[nodiscard]] auto check_program(const std::vector<parsed_module> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors)
    -> checked_types;

/// As above, reusing a module graph the caller already built (the
/// `validate_semantics` pipeline builds it for its own passes). The graph is
/// what module-rooted dotted paths (`pkg.mod.value`) are validated against.
[[nodiscard]] auto
check_program(const std::vector<parsed_module> &inputs,
              const module_session_index &session_index,
              const semantic_resolution_index &semantic_index,
              diagnostic_bag &diag, std::vector<bool> &file_has_errors)
    -> checked_types;

} // namespace kira::semantic
