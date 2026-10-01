#pragma once

#include <cstdint>

#include <vector>

#include "src/parser/diagnostic.h"
#include "src/semantic/analysis.h"
#include "src/semantic/ownership_cfg.h"
#include "src/semantic/types.h"

namespace cinder::semantic {

/// Enforces Cinder's ownership and borrowing rules
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

namespace ownership {

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
