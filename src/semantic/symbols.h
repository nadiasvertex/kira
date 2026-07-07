#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/semantic/ids.h"

namespace kira::semantic {

/// Which namespace a symbol's name is looked up in.
///
/// Kira keeps a few names disjoint (a type and a value may share a spelling)
/// so lookups must be namespace-qualified rather than name-only.
enum class symbol_namespace : uint8_t {
  module_type_namespace,     ///< Types, traits, concepts, and submodules.
  value_namespace,           ///< Functions, statics, and local bindings.
  type_parameter_namespace,  ///< In-scope generic type/value parameters.
  associated_type_namespace, ///< Associated types declared/defined in traits
                             ///< and impls.
};

/// What kind of declaration or binding a `semantic_symbol` represents.
enum class semantic_symbol_kind : uint8_t {
  type_symbol,            ///< `type` declaration.
  trait_symbol,           ///< `trait` declaration.
  concept_symbol,         ///< `concept` declaration.
  submodule_symbol,       ///< Nested `module` declaration.
  function_symbol,        ///< `def` declaration.
  static_binding_symbol,  ///< `static Name = expr` binding.
  parameter_symbol,       ///< Function or lambda parameter.
  local_binding_symbol,   ///< `let` binding.
  mutable_local_symbol,   ///< `var` binding.
  pattern_binding_symbol, ///< Name bound by a pattern (match arm, `if let`,
                          ///< `for`, ...).
  where_binding_symbol,   ///< Name bound by a trailing `where:` clause.
  type_parameter_symbol,  ///< Generic type/value parameter of a function, type,
                          ///< trait, impl, or concept.
  associated_type_symbol, ///< Associated type declared in a trait or defined in
                          ///< an impl.
};

/// One resolved symbol recorded in a `semantic_session`.
struct semantic_symbol {
  symbol_id id = k_invalid_symbol_id; ///< This symbol's own id.
  std::string name;                   ///< Declared or bound name.
  semantic_symbol_kind kind =
      semantic_symbol_kind::local_binding_symbol; ///< What kind of declaration
                                                  ///< this is.
  symbol_namespace name_space =
      symbol_namespace::value_namespace; ///< Namespace the name is looked up
                                         ///< in.
  std::string kind_name;                 ///< Cached display label for `kind`.
  ast::visibility visibility = ast::visibility::def; ///< Declared visibility.
  source_location location; ///< Where the symbol was declared.
  scope_id defining_scope =
      k_invalid_scope_id; ///< Scope the symbol was added to.
};

/// Inputs needed to add a `semantic_symbol` to a scope, before it has been
/// assigned a `symbol_id` or a `defining_scope`.
struct semantic_symbol_spec {
  std::string name;
  semantic_symbol_kind kind = semantic_symbol_kind::local_binding_symbol;
  symbol_namespace name_space = symbol_namespace::value_namespace;
  ast::visibility visibility = ast::visibility::def;
  source_location location;
};

/// Returns the stable human-readable label used to describe `kind` in
/// diagnostics (e.g. "type", "trait", "binding").
auto semantic_symbol_kind_name(semantic_symbol_kind kind) -> std::string_view;

/// Whether a symbol of `kind` participates in the "duplicate name in this
/// module scope" check (types, traits, concepts, and submodules only —
/// functions and value bindings are excluded because Kira allows a value and
/// a type to share a spelling).
auto participates_in_duplicate_module_scope_check(semantic_symbol_kind kind)
    -> bool;

/// Whether a symbol of `kind` can be the target of a named-type reference
/// (used when validating qualified type paths like `pkg.mod.Thing`).
auto can_resolve_named_type(semantic_symbol_kind kind) -> bool;

/// Whether a symbol of `kind` can be the target of a module-qualified value
/// reference (used when validating qualified paths like `pkg.mod.thing`).
auto can_resolve_module_reference(semantic_symbol_kind kind) -> bool;

/// Returns the stable human-readable label used to describe `name_space` in
/// diagnostics.
auto symbol_namespace_name(symbol_namespace name_space) -> std::string_view;

/// Builds the symbol spec for a module-scope item (`type`, `trait`,
/// `concept`, submodule, `def`, or a `static` binding), or `nullopt` if
/// `node` does not introduce a module-scope symbol.
auto module_symbol_spec(const ast::node &node, file_id_type file_id)
    -> std::optional<semantic_symbol_spec>;

} // namespace kira::semantic
