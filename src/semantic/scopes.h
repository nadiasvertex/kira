#pragma once

#include <string>
#include <vector>

#include "src/parser/source_location.h"
#include "src/semantic/ids.h"

namespace kira::semantic {

/// Kind of syntactic construct that introduced a `semantic_scope`.
///
/// Distinguishing scope kinds lets later passes reason about which names are
/// visible where without re-deriving that fact from the AST shape.
enum class semantic_scope_kind : uint8_t {
  module_scope,  ///< Top-level or nested `module` declaration.
  trait_scope,   ///< Body of a `trait` declaration.
  impl_scope,    ///< Body of an `impl` block.
  type_scope,    ///< Body/type-parameter scope of a `type` declaration.
  concept_scope, ///< Body of a `concept` declaration.
  function_signature_scope, ///< Parameter list and type parameters of a
                            ///< function.
  function_body_scope,      ///< Statement body of a function.
  lambda_signature_scope,   ///< Parameter list of a lambda expression.
  lambda_body_scope,        ///< Body of a lambda expression.
  block_scope,     ///< An ordinary indented block or `let`/`var` extension.
  branch_scope,    ///< Body of an `if`/`elif`/`else` branch.
  loop_scope,      ///< Body of a `while`/`for` loop or `for` comprehension.
  match_arm_scope, ///< Body and bindings of one `match` arm.
  where_scope,     ///< Bindings introduced by a trailing `where:` clause.
};

/// One lexical scope in the semantic scope tree.
///
/// Scopes form a tree via `parent`; `symbols` lists only the symbols declared
/// directly in this scope; ancestor scopes are consulted through
/// `resolve_symbol` for anything not found locally.
struct semantic_scope {
  scope_id id = k_invalid_scope_id; ///< This scope's own id.
  semantic_scope_kind kind =
      semantic_scope_kind::block_scope; ///< What introduced this scope.
  scope_id parent = k_invalid_scope_id; ///< Enclosing scope, if any.
  file_id_type file_id = 0;             ///< File the scope's syntax lives in.
  std::string module_name;  ///< Fully-qualified module the scope belongs to.
  std::string debug_name;   ///< Human-readable label for diagnostics/debugging.
  source_location location; ///< Source span the scope covers.
  std::vector<symbol_id> symbols; ///< Symbols declared directly in this scope.
};

/// Returns the stable human-readable label used to describe `kind` in
/// debugging output and diagnostics.
auto semantic_scope_kind_name(semantic_scope_kind kind) -> std::string_view;

} // namespace kira::semantic
