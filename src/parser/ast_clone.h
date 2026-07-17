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

/// Deep-clones a `type` declaration — visibility, modifiers, name, type
/// parameters, definition body (struct/sum/alias/refinement), `deriving`
/// list, and any invariant. Used to materialize a parameterized module's
/// `type` members per instantiation so each instantiation gets distinct node
/// identity while a projection resolves through the instantiation's
/// import-alias binding. Fails with a `clone_error` on a definition shape it
/// does not yet cover, mirroring the other cloners.
[[nodiscard]] auto clone_type_decl(const type_decl &decl)
    -> std::expected<ptr<type_decl>, clone_error>;

/// Deep-clones a `static` binding declaration (`static Name [: T] = expr`).
/// Only the `binding` form is supported — the assertion, conditional-
/// compilation, and `static for` forms fail with a `clone_error`, since a
/// parameterized module's required-constant members are always bindings.
[[nodiscard]] auto clone_static_decl(const static_decl &decl)
    -> std::expected<ptr<static_decl>, clone_error>;

/// Deep-clones an `impl` block — its type parameters, trait/`for` target type
/// expressions, `where` constraints, and member items (methods and
/// associated-type definitions). Used to materialize a parameterized module's
/// `impl` members per instantiation, so each instantiation's impl participates
/// in coherence and the method table with distinct node identity while its
/// target type resolves through the instantiation's bindings. Fails with a
/// `clone_error` on a member shape it does not yet cover.
[[nodiscard]] auto clone_impl_decl(const impl_decl &decl)
    -> std::expected<ptr<impl_decl>, clone_error>;

/// Deep-clones an `extend Type:` block — its `for` target type expression and
/// method items — for the same per-instantiation materialization reason as
/// `clone_impl_decl`.
[[nodiscard]] auto clone_extend_decl(const extend_decl &decl)
    -> std::expected<ptr<extend_decl>, clone_error>;

/// Deep-clones a `trait` declaration — visibility, name, type parameters,
/// `requires` bound, and member items (method signatures, statics, and
/// associated-type declarations) — for per-instantiation materialization of a
/// parameterized module's `trait` members.
[[nodiscard]] auto clone_trait_decl(const trait_decl &decl)
    -> std::expected<ptr<trait_decl>, clone_error>;

} // namespace kira::ast
