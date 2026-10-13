#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

#include "src/parser/ast.h"
#include "src/semantic/method_resolution.h"
#include "src/semantic/type_table.h"

namespace cinder::semantic::detail {

// ==========================================================================
//  Method instances — the rules for naming and deriving per-impl methods
//
//  A method reached through an impl is compiled once per receiver: named
//  for the impl target and the instance's answers, and, for a trait's
//  default body, derived for each impl that does not override it. The rules
//  for those two things are here. Making the instance (cloning, checking
//  under a context, queueing) is the checker's.
// ==========================================================================

/// The symbol an instance of `method` is compiled under: the impl target,
/// the method, and the solution's suffix (`option::pure$int32`,
/// `vec::at$3`). It carries the impl target because that is how HIR
/// lowering names an impl member.
[[nodiscard]] auto method_instance_name(std::string_view target,
                                        std::string_view method,
                                        std::string_view suffix)
    -> std::string;

/// What an impl does with one function a trait declares.
enum class trait_item_action {
  /// The impl writes its own, so the trait's is not inherited.
  overridden,
  /// No `self` receiver (an associated-function-shaped requirement) or no
  /// default body (a required method the impl must provide, validated
  /// elsewhere): registered as it stands, nothing to derive.
  register_as_declared,
  /// A default body callable on `self`: cloned and checked once per impl.
  derive_default,
};

/// Whether `decl`'s first parameter is `self`. `first_param_name` is that
/// parameter's spelling, or empty when it has none.
[[nodiscard]] auto classify_trait_item(
    const ast::func_decl &decl, std::string_view first_param_name,
    const std::unordered_set<std::string> &overridden_names)
    -> trait_item_action;

/// The bindings a trait default body is checked under for an impl whose
/// target is `target`. A higher-kinded trait's default is written against
/// the trait's constructor parameter (`F[A]`); binding that parameter to the
/// impl's target constructor makes every `F[A]` in the clone re-resolve to
/// the real applied type. Empty for any other trait or target.
[[nodiscard]] auto trait_default_bindings(const type_table &types,
                                          const ast::trait_decl *trait,
                                          type_id target) -> type_scope;

} // namespace cinder::semantic::detail
