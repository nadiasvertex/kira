#pragma once

#include <string>
#include <vector>

#include "src/parser/ast.h"
#include "src/parser/source_location.h"

namespace kira::semantic {

/// One name a pattern binds, in the order the pattern would bind it.
struct pattern_binding {
  std::string name;
  /// Declaring pattern node, when one exists. Null for a struct pattern's
  /// shorthand field (`{x}`, `ast::field_pattern.pattern == nullptr`) and a
  /// group pattern's alias (`(inner) as x`) — neither has a dedicated
  /// pattern node of its own to key a later lookup (e.g. into
  /// `checked_types::node_types`) against.
  const ast::pattern *node = nullptr;
  source_span span;
};

/// Recursively collects every name `pattern` would bind — including nested
/// tuple/struct/constructor/array subpatterns, option/result payloads,
/// ref-pattern payloads, or-pattern alternatives, and group-pattern aliases
/// — in the order the pattern would bind them.
[[nodiscard]] auto collect_pattern_bindings(const ast::pattern &pattern)
    -> std::vector<pattern_binding>;

} // namespace kira::semantic
