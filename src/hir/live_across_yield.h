#pragma once

#include <vector>

#include "src/hir/nodes.h"

namespace kira::hir {

/// Every local (parameter or `hir_let`/`hir_let_else`/`hir_while_let`/
/// `hir_match`-subject binding) in `fn.body` whose value must survive at
/// least one `hir_yield` — i.e. it's bound at or before some yield point
/// and referenced again at or after that same yield resumes — in
/// first-bound order, deduplicated.
///
/// This is the generator analog of `free_variables` (`captures.h`): where a
/// lambda's environment block holds every name free *into* the lambda, a
/// generator's state block holds every name that needs to survive *across*
/// a suspension of the function's own body. Only meaningful for a
/// `generator def`'s `hir_function` (`fn.is_generator`); callers should not
/// invoke this on an ordinary function.
///
/// A `hir_while`/`hir_while_let` loop whose body contains a `yield`
/// conservatively marks every symbol the loop's condition/subject and body
/// reference (that was bound before the loop) as live, since a suspended
/// loop must resume by re-evaluating its condition on the next iteration —
/// this can mark a few more locals live than strictly necessary but never
/// misses one. The same conservative merge applies across `hir_if`/
/// `hir_match` branches: a yield in one branch can make a symbol referenced
/// in a later branch appear live even though the branches are mutually
/// exclusive at runtime — safe (over-approximation), never unsound.
[[nodiscard]] auto live_across_yield(const hir_function &fn)
    -> std::vector<symbol_id>;

} // namespace kira::hir
