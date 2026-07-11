#pragma once

#include <cstdint>

#include "src/parser/ast.h"

namespace kira::comptime {

/// Renames every plain (non-destructuring) `let`/`var`/`where`-binding/
/// `crew` name introduced *inside* `fragment` — a quoted `expr`/`stmt`
/// fragment's parsed body, see `ast::quote_expr::parsed_body` — to a fresh,
/// globally-unique synthetic name, along with every reference to that
/// binding within the same fragment. This is the "uncaptured internal
/// bindings get a fresh name" half of hygiene (compile-time evaluation
/// design plan, section 5): without it, splicing `` `(let temp = 99)` ``
/// into a scope that already has its own `temp` bound would silently
/// clobber the splice site's own binding — `checker::check_body_node`'s
/// `splice_stmt` case reuses the enclosing scope rather than pushing a new
/// one, so a spliced `let` is otherwise indistinguishable from one written
/// directly at the splice site.
///
/// `next_id` is threaded in by the caller (`comptime::evaluator::eval_quote`
/// passes its own session-lifetime counter) rather than owned here, so
/// names stay unique across every fragment renamed in one compilation
/// session — two different quoted fragments, each with their own internal
/// `temp`, must never mint the same synthetic name, or they could collide
/// with *each other* once both are spliced into the same scope.
///
/// Deliberately narrow, per the design plan's own recommendation to
/// prototype the *narrowest* case first: only plain-name bindings are
/// renamed. Left unrenamed (documented, not silently mishandled) — a
/// binding introduced this way is simply not registered, so any reference
/// to it inside the fragment is left as free text, unaffected by this pass
/// (and still resolves against the *splice* site, exactly as before M5,
/// since resolving a quote's free references against its own *definition*
/// site is the other, still-unimplemented half of hygiene):
///   - Destructuring patterns in `let`/`var` (`let (a, b) = ...`, struct/
///     tuple/constructor patterns).
///   - `for`/`while let`/`match` pattern bindings.
///   - `lambda` parameters.
///   - Struct-literal shorthand field init (`{x}`) referencing a renamed
///     binding.
/// A nested `` `(...)` `` quote or `~splice` found inside `fragment` is
/// left completely untouched (not recursed into at all) — hygiene for
/// nested quotes is its own, harder problem, out of scope here.
///
/// Mutates `fragment` in place. Idempotent per node is the *caller's*
/// responsibility, not this function's — every call mints new names, so
/// `eval_quote` guards against renaming the same fragment twice.
void rename_internal_bindings(ast::node &fragment, std::uint64_t &next_id);

} // namespace kira::comptime
