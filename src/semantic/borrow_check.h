#pragma once

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/types.h"

namespace kira::semantic {

/// Enforces Kira's borrow discipline (`spec/specification/02-intermediate/
/// 14-ownership-and-borrowing.md`) over every checked function and lambda
/// body — the rules the move checker (`check_moves`) deliberately leaves to a
/// separate pass. Two things are checked:
///
///   * **No escape.** A `&`/`&mut` borrow lends a value for the duration of a
///     single call; it is not a value in its own right. So a borrow
///     expression whose result is a plain reference (`type_kind::ref_kind`)
///     is only legal as the direct argument of a call, the callee, or the
///     base of a field/index projection — passing positions. Anywhere it
///     would be *stored* instead — a `let`/`var` initializer, a `return`
///     value, an assignment right-hand side, or an element of a tuple, array,
///     struct literal, or collection — it cannot escape the call, and is
///     rejected. A `&mut container[a..b]` produces a *view* (`mut slice`),
///     not a `ref_kind`, so it is exempt: views are the one borrowing value
///     allowed to outlive a call (see the Views chapter), and this pass keys
///     off the borrow expression's recorded result type to tell them apart.
///
///   * **Exclusivity.** A borrow made as an argument of a call is live for the
///     whole of that call — including while the call's *other* arguments (and
///     anything nested in them) are evaluated. So two borrows are
///     simultaneously live whenever one call is nested inside the argument
///     evaluation of another, not only within one argument list. The walk
///     therefore threads the set of borrows made by the enclosing (still
///     in-progress) calls down each sub-expression, and checks each new borrow
///     against it. Because the escape rule forbids storing a borrow, no borrow
///     outlives the statement that created it, so this per-statement live set
///     is all the dataflow the check needs. Within the live set: any number of
///     `&` borrows may coexist, but at most one `&mut`, and a `&mut` may not
///     coexist with any `&`. Borrows are compared at whole-variable
///     (root-binding) granularity, matching the move checker; two borrows of
///     different fields of the same variable are conservatively rejected.
///
///     The participating borrows are the explicit `&`/`&mut` arguments, the
///     implicit autoref of a bare argument passed to a `&`/`&mut` parameter,
///     and the autoref of a method/UFCS receiver. A receiver borrow is
///     modelled two-phase (as in Rust): active when the call runs, but only
///     *reserved* while that call's own arguments are evaluated, during which
///     it tolerates a coexisting shared borrow of the same value (so
///     `xs.set(i, xs.get(j))` is legal) while still conflicting with a nested
///     `&mut` of it (so `xs.set(i, take(&mut xs))` is not). Autoref and
///     receiver borrows are only tracked for calls resolved to a real
///     declaration (`checked.resolved_callees`); a call through a plain
///     `fn(...)`-typed value is conservatively treated as borrowing nothing
///     implicitly, matching the move checker's policy.
///
/// `checked` must be the result of `check_program` over the same `inputs`;
/// this pass only trusts types it can look up in `checked.node_types`. Files
/// already in `file_has_errors`, and files at or above `skip_from_fileid`
/// (the injected stdlib prelude), are skipped — matching `check_moves`.
auto check_borrows(const std::vector<parsed_module> &inputs,
                   const checked_types &checked, diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors,
                   unsigned skip_from_fileid) -> void;

} // namespace kira::semantic
