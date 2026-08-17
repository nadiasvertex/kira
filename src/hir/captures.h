#pragma once

#include <unordered_set>
#include <vector>

#include "src/hir/nodes.h"

namespace kira::hir {

/// The free variables `lambda.body` references from an enclosing scope —
/// every `hir_local_ref` whose symbol isn't itself bound inside the lambda
/// (as a parameter, or via `hir_let`/`hir_let_else`/`hir_while_let`/
/// `hir_match`'s own subject bindings — pattern bindings are always
/// ordinary `hir_let`s per `hir_pattern`'s doc comment, so no separate case
/// is needed for those) — in first-reference order, deduplicated. A nested
/// lambda's body is walked too (its free variables may include names bound
/// in *this* lambda, which then need to flow through as part of this
/// lambda's own environment), but a nested lambda's own parameters don't
/// leak out as bindings visible to sibling code after it.
///
/// This is exactly the capture set both `bytecode_compiler` and
/// `llvm_codegen` build a closure's environment block from
/// (`spec/codegen-design.md` increment 6) — a lambda whose result is empty
/// captures nothing, and its closure value's `env_ptr` slot is null.
[[nodiscard]] auto free_variables(const hir_lambda &lambda)
    -> std::vector<symbol_id>;

/// The ordered environment plan for `lambda` — what both backends build the
/// closure's environment block from, and the ABI contract between the code
/// that *builds* the block and the code that *reads* it inside the body.
///
/// With an explicit capture list, that list in source order, carrying each
/// entry's declared mode. The list, not the body's reference order, is then
/// the layout: it is a closed, stated set, so a slot exists for every entry
/// whether the body reads it or not. Without a list, `free_variables` in
/// first-reference order with every entry `by_value` — the implicit
/// behavior that predates capture lists.
///
/// Callers still have to drop symbols that aren't locals of the enclosing
/// function: `free_variables` reports module-level functions too (an
/// ordinary call's callee is an `hir_local_ref`), and those resolve
/// directly at both call sites rather than through the environment.
[[nodiscard]] auto capture_plan(const hir_lambda &lambda)
    -> std::vector<hir_capture>;

/// Every local declared in `body` that some lambda within it captures by
/// reference, and which therefore cannot live in a plain value slot: a
/// by-reference capture needs somewhere with an address, so these have to be
/// boxed into a one-slot cell that both the enclosing frame and the closure
/// environment can point at.
///
/// The LLVM backend doesn't need this — an `alloca` already has an address.
/// The bytecode VM does: its locals are virtual registers.
///
/// Nested lambdas are walked, since a lambda two levels down can name a
/// local of this body; a lambda's own parameters are not, since those belong
/// to that lambda's frame rather than this one.
[[nodiscard]] auto ref_captured_symbols(const hir_block &body)
    -> std::unordered_set<symbol_id>;

} // namespace kira::hir
