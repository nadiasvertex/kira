#pragma once

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/types.h"

namespace kira::semantic {

/// Walks every checked function and lambda body, flagging a binding used
/// after it has already been moved from — the analysis Kira's scope-based
/// `drop` destructors (`spec/kira-reference.md`, "Destructors: drop") need
/// underneath them, since "a binding that was moved from never drops" and
/// reverse-declaration-order drop both require knowing which bindings are
/// still live at a given program point.
///
/// `checked` must be the result of `check_program` over the same `inputs`:
/// this pass only trusts types it can look up in `checked.node_types` (a
/// binding whose type was never resolved — inside a file `check_program`
/// never reached — is treated as untrackable rather than guessed at).
/// Files already in `file_has_errors` are skipped, matching `check_program`.
///
/// First-cut scope, deliberately: tracks whole-binding moves only, at the
/// syntactic sites the spec makes unambiguous — a bare identifier used as a
/// `let`/`var` initializer, `return` value, call argument, or general
/// expression operand moves it; the same identifier as the direct operand
/// of `&`/`&mut`, the base of a field/index projection, or the target of an
/// assignment does not (assignment to a `var` name instead un-moves it, a
/// fresh value having replaced the old one). Builtin scalar/boolean/unit
/// types never move. Not yet handled — left as follow-up work once this
/// lands: `shared[T]` refcount semantics, closures' borrow-vs-move capture
/// rule (every identifier reachable from inside a closure body is treated
/// like any other use), partial (field-level) moves, and CFG-level exit
/// tracking (panic unwinding, `?`, `break`/`continue`) beyond straight-line
/// code and structured `if`/`while`/`for`/`match`.
auto check_moves(const std::vector<parsed_module> &inputs,
                 const checked_types &checked, diagnostic_bag &diag,
                 std::vector<bool> &file_has_errors) -> void;

} // namespace kira::semantic
