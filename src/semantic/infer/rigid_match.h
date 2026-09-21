#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "src/semantic/infer/unify.h"
#include "src/semantic/types.h"

namespace kira::semantic::infer {

// ==========================================================================
//  Matching a declared pattern against a concrete type
//
//  Phase 7 of `spec/inference-rewrite.md`. This is the replacement for
//  `checker::unify_rigid` (`check.cpp`), the matcher all 28 of its call sites
//  and 67 binding maps run through.
//
//  What `unify_rigid` is, precisely: a structural walk that populates an
//  `unordered_map<string, type_id>` keyed by parameter *name*, never fails,
//  and silently returns on any mismatch keeping whatever it had bound so far.
//  That is not a unifier, and the places it falls short are not a list of
//  bugs to fix one at a time — they are what one mechanism was supposed to
//  prevent:
//
//    - An `array[T, n]` pattern unifies its element and ignores its *length*,
//      so `n` never solves from an argument.
//    - `&T` and `&mut T` match each other, because the mutability flag is not
//      consulted.
//    - A `&T` pattern matches a bare `T` and vice versa, an allowance copied
//      from `compatible` into a function whose job is solving, not coercion.
//    - Value slots are not solved at all, only carried.
//
//  So this file does not reimplement the walk. It hands the pattern and the
//  concrete type to the phase-2 `unifier`, which already decides all four
//  correctly, and translates at the edges: the pattern's type parameters are
//  adopted as metavariables of the store, and the solutions are read back out
//  under the names the call sites expect.
//
//  The differences were gated rather than sprung, because the phase 0 golden
//  has to stay readable while 28 sites move: `legacy_compat` reproduced
//  `unify_rigid`'s permissiveness so the swap itself was verifiable as a
//  no-op, and each allowance was then retired on its own. Two are already
//  gone. The one that remains is documented on the field.
// ==========================================================================

/// Which of `unify_rigid`'s allowances to keep for now.
///
/// Every field here is a known defect being held in place deliberately so
/// that one migration step changes one thing. A field that reaches zero call
/// sites should be deleted, not defaulted.
struct legacy_compat {
  /// A `&T` pattern matches a bare `T`, and a bare `T` pattern matches `&T`.
  ///
  /// The last one standing. `ignore_mutability` and `ignore_array_length`
  /// were retired the moment the swap landed — neither changed a recorded
  /// decision — but this one is load-bearing: the checker leans on the
  /// matcher to absorb the auto-borrow at call sites that nothing else
  /// performs, and switching it off fails `cli_test`, `std_test` and
  /// `move_check_test`. Retiring it means writing the explicit coercion step
  /// it stands in for.
  bool ref_coercion = false;
};

/// What a match produced.
struct rigid_match_result {
  /// The pattern's type parameters that solved, by the name they are spelled
  /// with. A parameter the concrete type does not pin is simply absent —
  /// callers distinguish "solved to `unknown`" from "not solved" by
  /// membership, exactly as they did with `unify_rigid`'s map.
  std::unordered_map<std::string, type_id> bindings;
  /// Set when the two genuinely disagree.
  ///
  /// `unify_rigid` had nowhere to put this and so dropped it, which is why a
  /// mismatched signature could reach elaboration as a half-solved binding
  /// map. Callers may still ignore it — the bindings made before the
  /// disagreement are in `bindings` either way — but it is no longer
  /// unavailable.
  std::optional<unify_error> failure;
};

/// Solves `pattern`'s type parameters against `concrete`.
///
/// `pattern` is a declared type that may mention `type_param_kind` ids (the
/// `T` and `n` of `def push[T](xs: list[T], v: T)`); `concrete` is what a
/// caller actually supplied. Nothing is recorded outside the call: the store
/// is local, which is what makes adopting interned parameter ids safe — see
/// `infer_ctxt::adopt`.
[[nodiscard]] auto match_pattern(type_table &table, type_id pattern,
                                 type_id concrete,
                                 const legacy_compat &compat = {})
    -> rigid_match_result;

} // namespace kira::semantic::infer
