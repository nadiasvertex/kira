#pragma once

#include "src/hir/nodes.h"

namespace kira::hir {

/// Marks every `hir_call` in `fn`'s body that is a tail call, per
/// spec/specification/03-advanced/39-tail-call-optimization.md: in tail
/// position, a direct call to a statically-known function (never a
/// closure/indirect call or an intrinsic), and not inside a generator step.
/// Sets `hir_call::is_tail_call` in place — computed once, right after
/// lowering, so the bytecode and LLVM backends both consume the same
/// verdict instead of re-deriving (and potentially disagreeing on) it.
///
/// A no-op for a generator function (`fn.is_generator`): its body doubles
/// as the generator step function's body, which the spec excludes outright.
///
/// Also a no-op for a function that owns an `uninit[T, N]` buffer: the
/// buffer lives in this frame and a callee may hold a pointer into it, so
/// the frame cannot be reused until the callee returns.
///
/// Does not descend into a nested `hir_lambda`'s body. A lambda compiles to
/// its own separate function, so its own tail calls are a distinct
/// question this milestone doesn't answer — and more importantly, a name
/// the lambda calls that resolves to a *captured* local (a closure passed
/// into the enclosing function) is indistinguishable, from inside the
/// lambda alone, from a genuine top-level function reference without
/// tracking capture provenance; leaving lambda bodies unmarked avoids
/// mismarking that case as a direct call. `mark_tail_calls` may later be
/// applied to lambda bodies as a followup once that provenance exists.
auto mark_tail_calls(hir_function &fn) -> void;

} // namespace kira::hir
