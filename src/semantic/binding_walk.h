#pragma once

#include <string>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"

namespace cinder::semantic {

/// One name a pattern binds, in the order the pattern would bind it.
struct pattern_binding {
  std::string name;
  /// Declaring pattern node, when one exists. Null for a struct pattern's
  /// shorthand field (`{x}`, `ast::field_pattern.pattern == nullptr`) and a
  /// group pattern's alias (`(inner) as x`) — neither has a dedicated
  /// pattern node of its own to key a later lookup (e.g. into
  /// `checked_types::node_types`) against.
  const ast::pattern *node = nullptr;
  /// The shorthand field (`{x}`) that declares the binding, if it is one.
  /// Its type is in `checked_types::struct_pattern_field_types`.
  const ast::field_pattern *field = nullptr;
  /// The group pattern whose alias (`(inner) as x`) the binding is, if it is
  /// one. The alias has the group pattern's type.
  const ast::group_pattern *alias_of = nullptr;
  source_span span;

  /// What identifies the binding's declaration: the pattern node, the
  /// shorthand field or the aliased group.
  [[nodiscard]] auto key() const -> const void * {
    if (node != nullptr) {
      return node;
    }
    if (field != nullptr) {
      return field;
    }
    return alias_of;
  }
};

/// Recursively collects every name `pattern` would bind — including nested
/// tuple/struct/constructor/array subpatterns, option/result payloads,
/// ref-pattern payloads, or-pattern alternatives, and group-pattern aliases
/// — in the order the pattern would bind them.
[[nodiscard]] auto collect_pattern_bindings(const ast::pattern &pattern)
    -> std::vector<pattern_binding>;

/// Whether every name `pattern` binds is its own copy of a distinct part of
/// the subject, so that owning each one owns the subject's parts exactly
/// once. False for a pattern with an alias (`(p) as x` binds the whole and
/// the parts), an or-pattern (alternatives bind the same names), a `&`
/// pattern (binds through a reference) or an array pattern (element and rest
/// bindings are not one slot each).
[[nodiscard]] auto pattern_bindings_are_disjoint(const ast::pattern &pattern)
    -> bool;

} // namespace cinder::semantic
