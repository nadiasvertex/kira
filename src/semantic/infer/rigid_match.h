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
//  has to stay readable while 28 sites move: a `legacy_compat` struct
//  reproduced `unify_rigid`'s permissiveness so the swap itself was verifiable
//  as a no-op, and each allowance was then retired on its own. All three are
//  now gone. The reference allowance was the only one that was not simply a
//  defect: it was standing in for a real rule of the language, which is now
//  written out as `coerce_at_call_site` rather than emulated by a matcher
//  that cannot see references.
// ==========================================================================

/// Applies the adjustments the language itself performs at a call site, so
/// the matcher does not have to be blind to see past them.
///
/// Two, and only at the outermost position:
///
///   - **Implicit borrow.** A `&self`/`&T` parameter accepts a value. This is
///     what `nums.iter()` relies on: `iter`'s receiver is declared `&list[T]`
///     and `nums` is a `list[int32]`, and without the borrow `T` never solves.
///   - **Implicit deref.** A by-value parameter accepts a reference, the
///     mirror of the same rule.
///
/// Outermost only, which is the difference between a coercion and a blind
/// spot. `list[&T]` against `list[int32]` is a real disagreement and stays
/// one; the old matcher's allowance fired at *every* depth and could not tell
/// the two apart. It also could not be expressed as a rewrite of the inputs:
/// a bare-parameter pattern swallows a whole `&int32`, so erasing references
/// everywhere binds `T := int32` where the rule binds `T := &int32`, and
/// `std.mem`'s view queries read the difference.
///
/// Returns the adjusted pair. Unchanged when no adjustment applies — the two
/// are both references, or neither is.
struct coerced_pair {
  type_id expected = k_unknown_type;
  type_id found = k_unknown_type;
  /// Whether an adjustment was applied at all.
  bool adjusted = false;
};
[[nodiscard]] auto coerce_at_call_site(const type_table &table,
                                       type_id expected, type_id found)
    -> coerced_pair;

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
                                 type_id concrete) -> rigid_match_result;

} // namespace kira::semantic::infer
