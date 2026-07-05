#pragma once

#include <vector>

#include "src/k-parser/diagnostic.h"
#include "src/semantic/analysis.h"

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
auto check_program(const std::vector<parsed_module> &inputs,
                   diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors) -> void;

} // namespace kira::semantic
