#pragma once

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/types.h"

namespace cinder::semantic {

/// Enforces Cinder's ownership and borrowing rules
/// (`spec/specification/02-intermediate/14-ownership-and-borrowing.md`,
/// `15-views.md`, `16-closures-and-capture.md`) over every function and
/// lambda body, as one analysis:
///
///   * **Use after move.** Passing a value by value, binding it, returning
///     it, or matching on it moves it; any later use on some path is
///     rejected. Scalars, raw pointers, references and shared views copy.
///   * **Exclusivity.** Every access to a local — read, write, move, borrow,
///     and the end of its scope — is checked against the borrows (*loans*)
///     of it still live at that point: any number of `&`, or one `&mut`; a
///     read conflicts with a live `&mut`; a write, move, or end of scope
///     conflicts with any live loan. Loans come from explicit `&`/`&mut`,
///     the implicit autoref of an argument or method receiver (two-phase for
///     a mutable receiver), `slice`/`cell` views made by indexing, and a
///     closure's `&`/`&mut` captures.
///   * **Loan lifetimes without annotations.** A loan is live exactly as long
///     as some value carrying it is live — a view binding, a struct storing
///     one, a closure, a call's in-flight arguments, a `for` loop's source,
///     or the function's return value. A call whose result can carry a
///     borrow conservatively carries every borrow its arguments made. So a
///     loan held by the returned value while the local it borrows goes out
///     of scope is a dangling borrow, reported as such.
///   * **No escape.** A plain `&`/`&mut` borrow is only written in a passing
///     position (a call argument, callee, projection base, `for` iterable,
///     or interpolation segment); stored anywhere else it is rejected, per
///     the spec.
///
/// Each body is lowered to a small control-flow graph of ownership events
/// (`ownership_cfg.h`) and checked with a backward liveness pass and forward
/// "which loans does each holder carry" and "maybe moved" passes, so loops,
/// early exits, and every pattern-binding form are handled uniformly.
/// Borrows are compared at whole-variable granularity.
///
/// `checked` must be the result of `check_program` over the same `inputs`.
/// Files already in `file_has_errors`, and files at or above
/// `skip_from_fileid` (the injected stdlib prelude), are skipped; a file with
/// an ownership error is marked in `file_has_errors`, so it never reaches
/// lowering.
auto check_ownership(const std::vector<parsed_module> &inputs,
                     const checked_types &checked, diagnostic_bag &diag,
                     std::vector<bool> &file_has_errors,
                     unsigned skip_from_fileid) -> void;

} // namespace cinder::semantic
