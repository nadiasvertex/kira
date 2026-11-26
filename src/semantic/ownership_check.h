#pragma once

#include <cstdint>
#include <set>

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/ownership_cfg.h"

namespace cinder::semantic {

/// Cinder's ownership and borrowing rules
/// (`spec/specification/02-intermediate/14-ownership-and-borrowing.md`,
/// `15-views.md`, `16-closures-and-capture.md`) over every function and
/// lambda body, as one analysis:
///
///   * **Use after move.** Passing a value by value, binding it, returning
///     it, or matching on it moves it; any later use on some path is
///     rejected. Scalars, raw pointers, `&T` references and shared views
///     copy.
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
///   * **References are values.** A `&`/`&mut` may be bound, stored, or
///     returned like a view, and is tracked the same way; returning a borrow
///     of one of the function's own locals is the dangling case above. A
///     `&mut T` is move-only, so it is never copied into a second live alias.
///
/// Each body is a small control-flow graph of ownership events
/// (`ownership_cfg.h`, built from the typed core by
/// `core::ownership_graphs`), checked with a backward liveness pass and
/// forward "which loans does each holder carry" and "maybe moved" passes, so
/// loops, early exits, and every pattern-binding form are handled
/// uniformly. A loan is rooted at the deepest field path of the place it
/// borrows, so borrows of different fields of one local do not conflict.
/// `core::check_ownership` runs it over a whole program.
namespace ownership {

/// Checks one graph and reports what breaks the rules into `diag`, as
/// diagnostics in `file_id`.
/// What checking one graph finds besides its diagnostics.
struct graph_result {
  /// For a function's own graph, what it does with its arguments' borrows,
  /// read off the state at its exit (`function_cfg::parameters`).
  function_summary summary;
  /// The closures, by `ast::lambda_expr`, that borrow a value by the
  /// borrow-or-move rule and outlive it: they escape, so they move it
  /// instead (ch. 16).
  std::set<const void *> escaping;
};

auto check_graph(const function_cfg &cfg, const checked_types &checked,
                 diagnostic_bag &diag, file_id_type file_id) -> graph_result;

/// Whether a local still holds its value at some point: on every path
/// (`owned`), on some paths only (`maybe_moved`, so dropping it needs a
/// run-time drop flag), or on none (`moved`).
enum class move_state : std::uint8_t { owned, maybe_moved, moved };

/// A `field_path` local below a dropped local that is not `owned` there.
struct moved_part {
  local_id local = 0;
  move_state state = move_state::moved;
};

/// A local a scope exit ends, and whether it still holds its value there.
/// `moved_parts` lists the
/// topmost `field_path` locals below it that may have been moved out; only
/// the fields outside them are surely still its to drop.
struct owned_local {
  local_id local = 0;
  move_state state = move_state::owned;
  std::vector<moved_part> moved_parts;
};

/// One `scope_exit_event` some path reaches, with the state of each local
/// it ends: what is still owned there, and so what must be dropped.
struct scope_exit_owned {
  const void *key = nullptr;
  std::vector<std::vector<owned_local>> groups;
};

/// One `assign_event` some path reaches: whether its target still holds a
/// value there (so the old one must be dropped first), and the field paths
/// below it that may have been moved out. `local` is `k_no_local` for a
/// place reached through a borrow, which always holds a value.
struct assign_owned {
  const void *key = nullptr;
  local_id local = k_no_local;
  move_state state = move_state::owned;
  std::vector<moved_part> moved_parts;
};

/// What drop scheduling needs from one body's "maybe moved" and "moved on
/// every path" passes: the owned locals at each scope exit, and the state
/// of each assignment's target.
struct drop_facts {
  std::vector<scope_exit_owned> exits;
  std::vector<assign_owned> assignments;
};

/// Runs the same forward "maybe moved" pass `check_ownership` uses for
/// use-after-move over `cfg`, plus its "moved on every path" counterpart,
/// and reads both at each scope exit and assignment.
[[nodiscard]] auto drop_facts_of(const function_cfg &cfg) -> drop_facts;

} // namespace ownership

} // namespace cinder::semantic
