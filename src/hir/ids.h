#pragma once

#include "src/semantic/ids.h"

namespace kira::hir {

/// Identity for a bound HIR-level declaration (parameter, `let`/`var`
/// binding, or function). Reused rather than reinvented (see
/// spec/typed-ir-design.md Decision 7): the semantic session already
/// assigns every binding a stable id, so HIR only needs to remember which
/// one a reference points at.
using symbol_id = semantic::symbol_id;

/// Sentinel meaning "no symbol" — mirrors `semantic::k_invalid_symbol_id`.
inline constexpr symbol_id k_invalid_symbol_id = semantic::k_invalid_symbol_id;

} // namespace kira::hir
