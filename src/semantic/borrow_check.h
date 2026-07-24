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
///     rejected. A `&mut container[a..b]` produces a *view* (`slice_mut`),
///     not a `ref_kind`, so it is exempt: views are the one borrowing value
///     allowed to outlive a call (see the Views chapter), and this pass keys
///     off the borrow expression's recorded result type to tell them apart.
///
///   * **Exclusivity within a call.** Because a borrow can never be stored,
///     two borrows are only ever simultaneously live within the argument list
///     of a single call — no dataflow is needed. Within one call: any number
///     of `&` borrows may coexist, but at most one `&mut`, and a `&mut` may
///     not coexist with any `&`. Borrows are compared at whole-variable
///     (root-binding) granularity, matching the move checker; two borrows of
///     different fields of the same variable are conservatively rejected.
///     v1 considers explicit `&`/`&mut` *arguments* only (not the implicit
///     autoref of a bare argument passed to a `&`/`&mut` parameter, nor a
///     method-call receiver).
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
