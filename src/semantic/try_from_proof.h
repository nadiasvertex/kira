#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "src/parser/ast.h"
#include "src/semantic/types.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  try_from proofs — the syntax a runtime proof compiles into
//
//  `index[8].try_from(x)` is rewritten into ordinary Cinder:
//
//      block:
//          let #proof0 = <x>
//          if <predicate with self := #proof0, parameters := constants>:
//              @some(#proof0)
//          else:
//              @none
//
//  This file builds that syntax. It reads nothing from the checker: the
//  checker decides *whether* a call is a proof and types the result, and
//  this builds and specializes the tree it types.
// ==========================================================================

/// The constant each of a refinement's value parameters was instantiated
/// with at this use. `nullopt` when one is still open (an `index[n]` inside
/// a function generic over `n`), in which case no runtime check can be
/// generated until the enclosing template is monomorphized.
auto refinement_constants(const type_table &types, const type_entry &entry)
    -> std::optional<std::unordered_map<std::string, int64_t>>;

/// Rewrites a cloned predicate so it speaks about *this* proof: `self`
/// becomes the local the value was bound to, and each of the refinement's
/// value parameters becomes its constant. Takes the owning pointer because
/// replacing `n` with `8` swaps one node for another.
auto specialize_predicate(
    ast::ptr<ast::expr> &slot, const std::string &value_binding,
    const std::unordered_map<std::string, int64_t> &parameters) -> void;

/// A synthesized identifier. Its span is exactly the name's width (one byte
/// wider for a variant's `@`), because `is_variant_ident` tells `@some`
/// from `some` by span width alone.
auto make_ident(std::string_view name, source_span span, bool is_variant)
    -> ast::ptr<ast::ident_expr>;

/// `@some(<binding>)`, or a bare `@none` when `binding` is empty.
auto make_variant_call(std::string_view variant, std::string_view binding,
                       source_span span) -> ast::ptr<ast::expr>;

/// Wraps an expression as a statement, the shape every block body wants.
auto wrap_in_stmt(ast::ptr<ast::expr> value) -> ast::ptr<ast::node>;

/// The rewritten proof, with handles to the nodes the checker must type.
/// Every pointer is owned by `block`.
struct proof_syntax {
  ast::ptr<ast::block_expr> block;
  /// The `let` binding's pattern.
  const ast::node *bound_pattern = nullptr;
  /// Stands in for the checked value; the checker redirects it to the
  /// expression the user wrote.
  const ast::ident_expr *placeholder = nullptr;
  const ast::expr *predicate = nullptr;
  const ast::expr *some_expr = nullptr;
  const ast::expr *none_expr = nullptr;
  const ast::expr *conditional = nullptr;
};

/// Builds the block described above from an already-specialized
/// `predicate`. `call_span` is the span of the `try_from` call and
/// `value_span` the span of its argument.
auto build_proof_syntax(ast::ptr<ast::expr> predicate,
                        const std::string &temp_name, source_span call_span,
                        source_span value_span) -> proof_syntax;

} // namespace cinder::semantic::detail
