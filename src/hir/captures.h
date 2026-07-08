#pragma once

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

} // namespace kira::hir
