#include "src/comptime/value.h"

#include <format>
#include <map>

#include "src/parser/ast_traversal.h"

namespace cinder::comptime {

namespace {

/// The shape of quoted syntax as text: every node's kind and the names,
/// literals and operators it carries. Two fragments built separately (one
/// by the interpreter, one for the VM) are the same syntax exactly when
/// their shapes are.
[[nodiscard]] auto fragment_shape(const ast::node &root) -> std::string {
  auto out = std::string{};
  ast::walk(root, [&out](const ast::node &node) -> bool {
    out += std::format("({}", static_cast<int>(node.kind));
    switch (node.kind) {
    case ast::node_kind::ident_expr:
      out += " " + ast::cast<const ast::ident_expr>(node).name;
      break;
    case ast::node_kind::literal_expr: {
      const auto &lit = ast::cast<const ast::literal_expr>(node);
      out += std::format(" {} {}", static_cast<int>(lit.lit_kind), lit.value);
      break;
    }
    case ast::node_kind::field_expr:
      out += " ." + ast::cast<const ast::field_expr>(node).field_name;
      break;
    case ast::node_kind::binary_expr:
      out += std::format(
          " op{}", static_cast<int>(ast::cast<const ast::binary_expr>(node).op));
      break;
    case ast::node_kind::named_type:
      for (const auto &segment :
           ast::cast<const ast::named_type>(node).path) {
        out += " " + segment;
      }
      break;
    case ast::node_kind::binding_pattern:
      out += " " + ast::cast<const ast::binding_pattern>(node).name;
      break;
    case ast::node_kind::constructor_pattern:
      out += " " + ast::cast<const ast::constructor_pattern>(node).name;
      break;
    case ast::node_kind::func_decl:
      out += " " + ast::cast<const ast::func_decl>(node).name;
      break;
    case ast::node_kind::interpolated_string_expr:
      for (const auto &segment :
           ast::cast<const ast::interpolated_string_expr>(node).segments) {
        out += segment.is_literal
                   ? std::format(" \"{}\"", segment.literal_text)
                   : std::format(" {{:{}}}", segment.has_spec
                                                 ? segment.spec.type_char
                                                       .value_or(' ')
                                                 : ' ');
      }
      break;
    default:
      break;
    }
    out += ")";
    return true;
  });
  return out;
}

} // namespace

auto same_value(const value &a, const value &b) -> bool {
  if (a.kind != b.kind) {
    return false;
  }
  const auto same_elements = [](const std::vector<value> &x,
                                const std::vector<value> &y) -> bool {
    if (x.size() != y.size()) {
      return false;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
      if (!same_value(x[i], y[i])) {
        return false;
      }
    }
    return true;
  };
  switch (a.kind) {
  case value_kind::unit:
  case value_kind::diagnostic_marker:
    return true;
  case value_kind::boolean:
    return a.boolean == b.boolean;
  case value_kind::integer:
    return a.integer == b.integer;
  case value_kind::floating:
    return a.floating == b.floating;
  case value_kind::string:
    return a.string == b.string;
  case value_kind::list:
    return same_elements(a.elements, b.elements);
  case value_kind::struct_instance: {
    // The interpreter leaves a struct literal with no type head unnamed, so
    // only the fields decide.
    if (a.fields.size() != b.fields.size()) {
      return false;
    }
    for (const auto &[name, field] : a.fields) {
      const auto other = b.fields.find(name);
      if (other == b.fields.end() || !same_value(field, other->second)) {
        return false;
      }
    }
    return true;
  }
  case value_kind::variant_instance:
    return a.variant_tag == b.variant_tag &&
           same_elements(a.elements, b.elements);
  case value_kind::closure:
    return a.function == b.function;
  case value_kind::type_value:
    return a.type_name == b.type_name && a.type_decl == b.type_decl;
  case value_kind::lambda:
    return a.fragment == b.fragment;
  case value_kind::expr_fragment:
  case value_kind::stmt_fragment:
  case value_kind::def_expr_fragment:
  case value_kind::type_expr_fragment:
  case value_kind::pattern_fragment:
    return a.fragment == b.fragment ||
           (a.fragment != nullptr && b.fragment != nullptr &&
            fragment_shape(*a.fragment) == fragment_shape(*b.fragment));
  }
  return false;
}

auto describe_value(const value &v) -> std::string {
  const auto join = [](const std::vector<value> &elements) -> std::string {
    auto out = std::string{};
    for (std::size_t i = 0; i < elements.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += describe_value(elements[i]);
    }
    return out;
  };
  switch (v.kind) {
  case value_kind::unit:
    return "()";
  case value_kind::boolean:
    return v.boolean ? "true" : "false";
  case value_kind::integer:
    return std::to_string(v.integer);
  case value_kind::floating:
    return std::format("{}", v.floating);
  case value_kind::string:
    return std::format("\"{}\"", v.string);
  case value_kind::list:
    return std::format("[{}]", join(v.elements));
  case value_kind::struct_instance: {
    // Sorted, so the same struct always reads the same.
    const auto sorted =
        std::map<std::string, value>(v.fields.begin(), v.fields.end());
    auto out = v.type_name.empty() ? std::string("{") : v.type_name + " {";
    auto first = true;
    for (const auto &[name, field] : sorted) {
      out += std::format("{}{}: {}", first ? "" : ", ", name,
                         describe_value(field));
      first = false;
    }
    return out + "}";
  }
  case value_kind::variant_instance:
    return v.elements.empty()
               ? std::format("@{}", v.variant_tag)
               : std::format("@{}({})", v.variant_tag, join(v.elements));
  case value_kind::closure:
    return std::format("the function `{}`", v.type_name);
  case value_kind::lambda:
    return "a lambda";
  case value_kind::type_value:
    return std::format("the type `{}`", v.type_name);
  case value_kind::expr_fragment:
  case value_kind::stmt_fragment:
  case value_kind::def_expr_fragment:
  case value_kind::type_expr_fragment:
  case value_kind::pattern_fragment:
    return "quoted syntax";
  case value_kind::diagnostic_marker:
    return "an error";
  }
  return "a value";
}

} // namespace cinder::comptime
