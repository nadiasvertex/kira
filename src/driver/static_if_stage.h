#pragma once

#include <vector>

#include "parse_stage.h"
#include "src/parser/diagnostic.h"

namespace kira::driver {

/// Folds import-gating `static if` blocks in every parsed file *before* the
/// module graph is built (`spec/module-values-design.md` §6). A top-level
/// `static if <cond>: ... use ...` makes the module graph depend on a
/// compile-time value, so its condition must be *early-evaluable* — foldable
/// from literals alone, with no name resolution (the only resolution-free
/// evaluation available this early). When the condition folds to a boolean,
/// the taken branch's items replace the `static if` node inline (and the
/// untaken branch is dropped), so `build_program_index` and every later phase
/// see exactly the selected `use`s. When it does not fold, the `static if` is
/// left in place and — because it gates a `use` the module graph would
/// otherwise miss — a diagnostic explains the restriction.
///
/// Only `static if` blocks that contain a `use` in a branch are touched; every
/// other `static if` keeps its ordinary check-time branch-selection behavior.
/// The taken branch is folded recursively, so a nested import-gating
/// `static if` inside it is handled too.
///
/// @param inputs Parsed files whose `ast_file->items` are rewritten in place.
/// @param diagnostics Bag accumulating restriction diagnostics.
auto fold_static_if_imports(std::vector<parsed_input> &inputs,
                            diagnostic_bag &diagnostics) -> void;

} // namespace kira::driver
