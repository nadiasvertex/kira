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
/// original, since a clone is never itself a diagnostic source — a diagnostic
/// about a clone points at the source the user actually wrote, which is what
/// makes `semantic::checker::emit_diag`'s deduplication both possible and
/// necessary).
///
/// Two callers in `semantic::check.cpp`, both re-checking one written body
/// under a substitution the original couldn't be checked under:
/// `build_method_table` clones an unwritten trait-default method per concrete
/// impl, so the clone can be checked with `self` bound to that impl's target
/// rather than the trait's abstract placeholder; `instantiate_const_generic`
/// clones a const-generic template per constant it is called with, so the
/// clone can be checked with `n` bound to a number rather than to itself.
///
/// Covers the ordinary function surface — declarations, control flow,
/// patterns, and expressions — but not the whole AST: a `where` clause, an
/// `async` function, closures, and the compile-time constructs (`quote`,
/// splices, `static`) fail with a `clone_error` naming the construct rather
/// than being silently dropped. A caller reports that error against the use
/// that needed the copy; extend the relevant `clone_*` function here when a
/// body legitimately needs more.
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
