#pragma once

#include <vector>

#include "parse_stage.h"
#include "src/parser/diagnostic.h"

namespace kira::driver {

/// Folds every module-scope `static if` block whose condition is
/// early-evaluable — foldable from literals alone, with no name resolution —
/// in every parsed file *before* the module graph and module-scope symbol
/// table are built (`spec/module-values-design.md` §6). When the condition
/// folds to a boolean, the taken branch's items replace the `static if` node
/// inline (and the untaken branch is dropped), so `build_program_index`,
/// module-scope symbol registration, and every later phase see exactly the
/// selected items — this is what makes a `static if` that picks between two
/// top-level declarations of the same name (e.g. `static if ...: type word =
/// int64 else: type word = int32`) register that name at all: module-scope
/// symbol registration never looks inside an unfolded `static if` node.
///
/// When the condition does not fold this early, the `static if` is left in
/// place for ordinary check-time branch selection
/// (`checker::resolve_static_if_branch`), which has full name/type
/// resolution available — *unless* the block gates a `use` in one of its
/// branches, in which case the module graph has no later chance to see the
/// selection, so it is kept with a restriction diagnostic instead.
///
/// The taken branch is folded recursively, so a nested `static if` inside it
/// is handled too, and folding also recurses into non-functor inline
/// submodule bodies.
///
/// @param inputs Parsed files whose `ast_file->items` are rewritten in place.
/// @param diagnostics Bag accumulating restriction diagnostics.
auto fold_static_if_imports(std::vector<parsed_input> &inputs,
                            diagnostic_bag &diagnostics) -> void;

} // namespace kira::driver
