#include "binding_walk.h"

#include <algorithm>

#include "src/parser/ast_traversal.h"

namespace cinder::semantic {
namespace {

auto collect(const ast::node &node, std::vector<pattern_binding> &out) -> void {
  if (!ast::is_pattern_kind(node.kind)) {
    return;
  }
  switch (node.kind) {
  case ast::node_kind::binding_pattern: {
    const auto &binding = ast::cast<const ast::binding_pattern>(node);
    if (!binding.name.empty()) {
      out.push_back(
          pattern_binding{.name = binding.name,
                          .node = &ast::cast<const ast::pattern>(node),
                          .span = binding.span});
    }
    return;
  }

  case ast::node_kind::struct_pattern: {
    const auto &struct_pattern = ast::cast<const ast::struct_pattern>(node);
    for (const auto &field : struct_pattern.fields) {
      if (field.is_rest) {
        continue;
      }
      if (field.pattern != nullptr) {
        collect(*field.pattern, out);
        continue;
      }
      if (!field.name.empty()) {
        out.push_back(pattern_binding{
            .name = field.name, .field = &field, .span = field.span});
      }
    }
    return;
  }

  case ast::node_kind::group_pattern: {
    const auto &group = ast::cast<const ast::group_pattern>(node);
    if (group.inner != nullptr) {
      collect(*group.inner, out);
    }
    if (group.alias.has_value()) {
      out.push_back(pattern_binding{
          .name = *group.alias, .alias_of = &group, .span = group.span});
    }
    return;
  }

  default:
    // Tuple, constructor, array, option, result, ref and or patterns bind
    // exactly what their sub-patterns bind.
    ast::for_each_child(
        node, [&out](const ast::node &child) { collect(child, out); });
    return;
  }
}

auto disjoint(const ast::node *node) -> bool {
  if (node == nullptr) {
    return true;
  }
  switch (node->kind) {
  case ast::node_kind::tuple_pattern: {
    const auto &tuple = ast::cast<const ast::tuple_pattern>(*node);
    return std::ranges::all_of(tuple.elements,
                               [](const auto &e) { return disjoint(e.get()); });
  }
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = ast::cast<const ast::constructor_pattern>(*node);
    return std::ranges::all_of(ctor.args,
                               [](const auto &e) { return disjoint(e.get()); });
  }
  case ast::node_kind::struct_pattern: {
    const auto &fields = ast::cast<const ast::struct_pattern>(*node).fields;
    return std::ranges::all_of(fields, [](const auto &f) {
      return f.is_rest || disjoint(f.pattern.get());
    });
  }
  case ast::node_kind::option_pattern:
    return disjoint(ast::cast<const ast::option_pattern>(*node).inner.get());
  case ast::node_kind::result_pattern:
    return disjoint(ast::cast<const ast::result_pattern>(*node).inner.get());
  case ast::node_kind::group_pattern: {
    const auto &group = ast::cast<const ast::group_pattern>(*node);
    return !group.alias.has_value() && disjoint(group.inner.get());
  }
  case ast::node_kind::or_pattern:
  case ast::node_kind::ref_pattern:
  case ast::node_kind::array_pattern:
    return false;
  default:
    return true;
  }
}

} // namespace

auto pattern_bindings_are_disjoint(const ast::pattern &pattern) -> bool {
  return disjoint(&pattern);
}

auto collect_pattern_bindings(const ast::pattern &pattern)
    -> std::vector<pattern_binding> {
  auto out = std::vector<pattern_binding>{};
  collect(pattern, out);
  return out;
}

} // namespace cinder::semantic
