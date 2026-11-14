#include "src/hir/abi.h"

#include <string>
#include <utility>
#include <vector>

namespace cinder::hir {

namespace {

[[nodiscard]] auto classify_layout(const runtime::value_layout &layout,
                                   semantic::type_id type) -> abi_value {
  auto value =
      abi_value{.type = type, .size = layout.size, .align = layout.align};
  if (layout.size == 0) {
    value.cls = pass_class::zero_sized;
    return value;
  }
  switch (layout.kind) {
  case runtime::value_layout_kind::scalar:
    value.cls = pass_class::scalar;
    break;
  case runtime::value_layout_kind::address:
    value.cls = pass_class::address;
    break;
  case runtime::value_layout_kind::handle:
    value.cls = pass_class::handle;
    break;
  case runtime::value_layout_kind::descriptor:
    value.cls = pass_class::descriptor;
    break;
  case runtime::value_layout_kind::closure:
  case runtime::value_layout_kind::structure:
  case runtime::value_layout_kind::tuple:
  case runtime::value_layout_kind::array:
  case runtime::value_layout_kind::sum:
    value.cls = pass_class::aggregate;
    break;
  }
  return value;
}

} // namespace

auto pass_class_name(pass_class cls) -> const char * {
  switch (cls) {
  case pass_class::zero_sized:
    return "zero_sized";
  case pass_class::scalar:
    return "scalar";
  case pass_class::address:
    return "address";
  case pass_class::handle:
    return "handle";
  case pass_class::descriptor:
    return "descriptor";
  case pass_class::aggregate:
    return "aggregate";
  }
  return "?";
}

auto abi_signature::operands() const -> std::vector<abi_operand> {
  auto out = std::vector<abi_operand>{};
  if (takes_environment) {
    out.push_back(abi_operand{.role = abi_operand_role::environment,
                              .value = abi_value{.cls = pass_class::address,
                                                 .size = runtime::k_word_size,
                                                 .align = runtime::k_word_size},
                              .by_address = false});
  }
  if (returns_through_address()) {
    out.push_back(abi_operand{.role = abi_operand_role::result_address,
                              .value = result,
                              .by_address = true});
  }
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].cls == pass_class::zero_sized) {
      continue;
    }
    out.push_back(abi_operand{.role = abi_operand_role::argument,
                              .source_index = i,
                              .value = params[i],
                              .by_address = is_indirect(params[i].cls)});
  }
  return out;
}

auto abi_signature::agrees_with(const abi_signature &other) const -> bool {
  const auto same_shape = [](const abi_value &a, const abi_value &b) {
    return a.cls == b.cls && a.size == b.size && a.align == b.align;
  };
  if (!same_shape(result, other.result) ||
      params.size() != other.params.size()) {
    return false;
  }
  for (size_t i = 0; i < params.size(); ++i) {
    if (!same_shape(params[i], other.params[i])) {
      return false;
    }
  }
  return true;
}

auto classify_value(runtime::value_layouts &layouts, semantic::type_id type)
    -> std::expected<abi_value, runtime::layout_error> {
  const auto layout = layouts.layout(type);
  if (!layout.has_value()) {
    return std::unexpected(layout.error());
  }
  return classify_layout(**layout, type);
}

auto classify_signature(runtime::value_layouts &layouts,
                        std::span<const semantic::type_id> params,
                        semantic::type_id result)
    -> std::expected<abi_signature, runtime::layout_error> {
  auto signature = abi_signature{};
  const auto classified_result = classify_value(layouts, result);
  if (!classified_result.has_value()) {
    return std::unexpected(classified_result.error());
  }
  signature.result = *classified_result;
  for (const auto param : params) {
    const auto classified = classify_value(layouts, param);
    if (!classified.has_value()) {
      return std::unexpected(classified.error());
    }
    signature.params.push_back(*classified);
  }
  return signature;
}

auto classify_fn_type(runtime::value_layouts &layouts,
                      const semantic::type_table &types,
                      semantic::type_id fn_type)
    -> std::expected<abi_signature, runtime::layout_error> {
  const auto &entry = types.entry(types.strip_refinement(fn_type));
  if (entry.kind != semantic::type_kind::fn_kind) {
    return std::unexpected(runtime::layout_error{
        .kind = runtime::layout_error_kind::unsupported,
        .message = "`" + types.display(fn_type) + "` is not a function type"});
  }
  return classify_signature(layouts, entry.args, entry.result);
}

auto classify_function(runtime::value_layouts &layouts,
                       const hir_function &function)
    -> std::expected<abi_signature, runtime::layout_error> {
  auto params = std::vector<semantic::type_id>{};
  params.reserve(function.params.size());
  for (const auto &param : function.params) {
    params.push_back(param.type);
  }
  return classify_signature(layouts, params, function.return_type);
}

auto format_signature(const abi_signature &signature) -> std::string {
  auto out = std::string{"("};
  auto first = true;
  const auto add = [&](const std::string &text) {
    if (!first) {
      out += ", ";
    }
    first = false;
    out += text;
  };
  if (signature.takes_environment) {
    add("env@");
  }
  if (signature.returns_through_address()) {
    add("result@");
  }
  for (const auto &param : signature.params) {
    add(pass_class_name(param.cls));
  }
  out += ") -> ";
  out += pass_class_name(signature.result.cls);
  return out;
}

} // namespace cinder::hir
