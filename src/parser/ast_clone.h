#pragma once

#include <expected>
#include <string>

#include "ast.h"
#include "source_location.h"

namespace kira::ast {

/// The first unsupported construct encountered while cloning — see
/// `clone_func_decl`'s doc comment for what "unsupported" means here.
struct clone_error {
  source_span span;
  std::string message;
};

/// Deep-clones `decl`'s full signature and body (spans copied as-is from the
/// original, since a clone is never itself a diagnostic source — see
/// `semantic::check.cpp`'s `build_method_table`, the only caller: it clones
/// an unwritten trait-default method per concrete impl so the clone can be
/// type-checked with `self_type_` bound to that impl's concrete target,
/// instead of the trait's own abstract placeholder).
///
/// Covers only the node kinds reachable from `std.io`'s `reader`/`writer`
/// trait-default bodies — a deliberately bounded set, not general AST
/// cloning. Anything outside that set (generics, contracts, `if`/`while
/// let`, closures, most expression/statement/type kinds) fails with a
/// `clone_error` naming the unsupported construct rather than silently
/// dropping it; extend the relevant `clone_*` function in `ast_clone.cpp`
/// if a future trait-default body needs more.
[[nodiscard]] auto clone_func_decl(const func_decl &decl)
    -> std::expected<ptr<func_decl>, clone_error>;

/// Deep-clones a single expression, over the same bounded node set
/// `clone_func_decl` covers (identifiers, literals, operators, field access,
/// indexing, calls, `?`, array literals) — and failing, rather than silently
/// dropping, on anything outside it.
///
/// Exposed for `semantic::check.cpp`'s refinement `try_from` desugaring
/// (`spec/dependent-types-design.md` §7), which has to rewrite a
/// refinement's `where` predicate to speak about a concrete value rather than
/// `self`, and cannot mutate the declaration's own AST to do it — several
/// call sites share that one predicate node, and each needs its own copy.
[[nodiscard]] auto clone_expr(const expr &e)
    -> std::expected<ptr<expr>, clone_error>;

} // namespace kira::ast
