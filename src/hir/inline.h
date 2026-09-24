#pragma once

#include <cstddef>

#include "src/hir/nodes.h"
#include "src/semantic/types.h"

namespace kira::hir {

/// What `inline_small_calls` did, for tests and `--show-compile-details`.
struct inline_stats {
  /// Call sites replaced by a copy of their callee's body.
  size_t call_sites = 0;
};

/// Replaces direct calls to small functions with a copy of the callee's
/// body, across every module in `modules` — a user module's `xs[i]` inlines
/// `std.list`'s `at` exactly as it would a function declared next to it.
///
/// This is how a library collection costs what a builtin did: since
/// `list[T]` moved into the standard library, every `xs[i]`/`xs.push(v)`
/// is a real call, and on the bytecode VM each call is a frame. Inlining in
/// HIR, before either backend runs, removes the frame on both tiers without
/// either one knowing it happened, and helps every small function — not
/// just `list`'s — the same way.
///
/// A call site becomes a `hir_block` that binds each argument to a fresh
/// `hir_let` (so arguments are still evaluated once, left to right) and then
/// runs the callee's body, whose `return`s are rewritten into the block's
/// tail value. A callee is inlined only when that rewrite is exact:
///
///   - its body is small (a fixed node budget) and is not a generator;
///   - every `return` is either the last statement or ends every branch of
///     an `if` whose code after it can move into that `if`'s `else` — the
///     guard-clause shape (`if bad: return x` then the real work) covers
///     the collection methods this exists for;
///   - it contains no lambda, `yield`, contract check or `uninit` buffer,
///     and never assigns to one of its own parameters;
///   - each argument's type matches its parameter's representation.
///
/// Anything else stays a call — declining is always correct. Every symbol
/// in a copied body is renamed to one fresh within the caller (symbols are
/// minted per function, so a callee's ids collide with the caller's), and a
/// function the callee referred to by bare name is re-qualified with the
/// callee's module, since the copy no longer lives there. Copies are made
/// from each callee's body as lowered, never from an already-expanded one,
/// so the result does not depend on the order functions are visited in;
/// nested calls inside a copy are expanded to a bounded depth, never into a
/// function already being expanded. Lambda bodies in the caller are left
/// alone. Tail-call marks are recomputed for every function that changed.
auto inline_small_calls(ptr_vec<hir_module> &modules,
                        const semantic::type_table &types) -> inline_stats;

} // namespace kira::hir
