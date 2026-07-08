#include "binding_walk.h"

namespace kira::semantic {
namespace {

auto collect(const ast::pattern &pattern, std::vector<pattern_binding> &out)
    -> void {
  switch (pattern.kind) {
  case ast::node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
    if (!binding.name.empty()) {
      out.push_back(pattern_binding{
          .name = binding.name, .node = &pattern, .span = binding.span});
    }
    return;
  }

  case ast::node_kind::tuple_pattern: {
    const auto &tuple = dynamic_cast<const ast::tuple_pattern &>(pattern);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        collect(*element, out);
      }
    }
    return;
  }

  case ast::node_kind::constructor_pattern: {
    const auto &ctor = dynamic_cast<const ast::constructor_pattern &>(pattern);
    for (const auto &arg : ctor.args) {
      if (arg != nullptr) {
        collect(*arg, out);
      }
    }
    return;
  }

  case ast::node_kind::struct_pattern: {
    const auto &struct_pattern =
        dynamic_cast<const ast::struct_pattern &>(pattern);
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
            .name = field.name, .node = nullptr, .span = field.span});
      }
    }
    return;
  }

  case ast::node_kind::array_pattern: {
    const auto &array = dynamic_cast<const ast::array_pattern &>(pattern);
    for (const auto &element : array.elements) {
      if (element != nullptr) {
        collect(*element, out);
      }
    }
    return;
  }

  case ast::node_kind::option_pattern: {
    const auto &option = dynamic_cast<const ast::option_pattern &>(pattern);
    if (option.inner != nullptr) {
      collect(*option.inner, out);
    }
    return;
  }

  case ast::node_kind::result_pattern: {
    const auto &result = dynamic_cast<const ast::result_pattern &>(pattern);
    if (result.inner != nullptr) {
      collect(*result.inner, out);
    }
    return;
  }

  case ast::node_kind::ref_pattern: {
    const auto &ref = dynamic_cast<const ast::ref_pattern &>(pattern);
    if (ref.inner != nullptr) {
      collect(*ref.inner, out);
    }
    return;
  }

  case ast::node_kind::or_pattern: {
    const auto &or_pattern = dynamic_cast<const ast::or_pattern &>(pattern);
    for (const auto &alternative : or_pattern.alternatives) {
      if (alternative != nullptr) {
        collect(*alternative, out);
      }
    }
    return;
  }

  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner != nullptr) {
      collect(*group.inner, out);
    }
    if (group.alias.has_value()) {
      out.push_back(pattern_binding{
          .name = *group.alias, .node = nullptr, .span = group.span});
    }
    return;
  }

  default:
    return;
  }
}

} // namespace

auto collect_pattern_bindings(const ast::pattern &pattern)
    -> std::vector<pattern_binding> {
  auto out = std::vector<pattern_binding>{};
  collect(pattern, out);
  return out;
}

} // namespace kira::semantic
