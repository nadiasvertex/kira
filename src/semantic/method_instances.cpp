#include "src/semantic/method_instances.h"

#include <format>

namespace cinder::semantic::detail {

auto method_instance_name(std::string_view target, std::string_view method,
                          std::string_view suffix) -> std::string {
  return std::format("{}::{}{}", target, method, suffix);
}

auto classify_trait_item(
    const ast::func_decl &decl, std::string_view first_param_name,
    const std::unordered_set<std::string> &overridden_names)
    -> trait_item_action {
  if (overridden_names.contains(decl.name)) {
    return trait_item_action::overridden;
  }
  const auto has_self = !decl.params.empty() && first_param_name == "self";
  const auto has_body = decl.body_expr != nullptr || !decl.body_stmts.empty();
  return has_self && has_body ? trait_item_action::derive_default
                              : trait_item_action::register_as_declared;
}

auto trait_default_bindings(const type_table &types,
                            const ast::trait_decl *trait, type_id target)
    -> type_scope {
  auto bound = type_scope{};
  if (trait != nullptr && !trait->type_params.empty() &&
      trait->type_params.front().higher_kinded_arity > 0 &&
      types.entry(target).kind == type_kind::ctor_ref_kind) {
    bound.emplace(trait->type_params.front().name, target);
  }
  return bound;
}

} // namespace cinder::semantic::detail
