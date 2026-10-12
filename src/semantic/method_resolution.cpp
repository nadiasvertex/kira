#include "src/semantic/method_resolution.h"

#include <algorithm>

namespace cinder::semantic::detail {

auto method_resolver::mentions_param(type_id id) const -> bool {
  const auto entry = types_->entry(id);
  if (entry.kind == type_kind::type_param_kind ||
      entry.kind == type_kind::param_app_kind) {
    return true;
  }
  return std::ranges::any_of(entry.args, [&](type_id arg) -> bool {
    return mentions_param(arg);
  });
}

auto method_resolver::target_pattern_matches(type_id pattern,
                                             type_id concrete) const -> bool {
  if (pattern == concrete || types_->is_unknown(pattern)) {
    return true;
  }
  const auto pattern_entry = types_->entry(pattern);
  if (pattern_entry.kind == type_kind::type_param_kind ||
      pattern_entry.kind == type_kind::param_app_kind ||
      pattern_entry.kind == type_kind::ctor_ref_kind ||
      pattern_entry.kind == type_kind::symbolic_value_kind) {
    // A value-kind impl parameter embedded in the target (`buf[n]`'s `n`)
    // interns the same way `type_param_kind` does for a type parameter —
    // as a placeholder the receiver is free to instantiate however it
    // likes. This is only a candidate filter; whether the receiver's
    // actual value is one `match_value_params` can pin down
    // is decided afterward, by `check_impl_generic_method_call`.
    return true;
  }
  const auto concrete_entry = types_->entry(concrete);
  if (pattern_entry.decl != concrete_entry.decl) {
    return false;
  }
  if (pattern_entry.decl == nullptr &&
      pattern_entry.name != concrete_entry.name) {
    return false;
  }
  if (pattern_entry.args.size() != concrete_entry.args.size()) {
    return false;
  }
  for (size_t i = 0; i < pattern_entry.args.size(); ++i) {
    if (!target_pattern_matches(pattern_entry.args[i], concrete_entry.args[i])) {
      return false;
    }
  }
  return true;
}

auto method_resolver::resolve(type_id receiver, std::string_view name,
                              const method_filter &accepts) const
    -> method_resolution {
  return resolve(types_->entry(receiver), receiver, name, accepts);
}

auto method_resolver::resolve(const type_entry &entry, type_id instance,
                              std::string_view name,
                              const method_filter &accepts) const
    -> method_resolution {
  if (entry.decl != nullptr) {
    if (const auto *found = resolve_declared(entry, instance, name, accepts)) {
      return {method_origin::declared, found};
    }
    return {};
  }
  // The impl or `extend` method is looked up even when the compiler defines
  // the name itself, because a caller that wants a declaration to read
  // (a static call's signature, a receiver's `deref`) is not asking which
  // one a method call would dispatch to.
  const auto *impl = resolve_builtin_impl(entry, name);
  const auto *extend = impl == nullptr ? resolve_builtin_extend(entry, name)
                                       : nullptr;
  const auto *declared = impl != nullptr ? impl : extend;
  if (has_inherent_ != nullptr && has_inherent_(entry, name)) {
    return {method_origin::builtin_inherent, declared};
  }
  if (impl != nullptr) {
    return {method_origin::builtin_impl, impl};
  }
  if (extend != nullptr) {
    return {method_origin::builtin_extend, extend};
  }
  return {};
}

auto method_resolver::declared_names(const type_entry &entry) const
    -> std::vector<std::string> {
  auto names = std::vector<std::string>{};
  if (entry.decl != nullptr) {
    if (const auto it = tables_->declared.find(entry.decl);
        it != tables_->declared.end()) {
      for (const auto &method : it->second) {
        names.push_back(method.decl->name);
      }
    }
  }
  if (const auto it = tables_->extend_by_builtin.find(entry.name);
      entry.decl == nullptr && it != tables_->extend_by_builtin.end()) {
    for (const auto &method : it->second) {
      names.push_back(method.decl->name);
    }
  }
  return names;
}

auto method_resolver::resolve_declared(const type_entry &entry,
                                       type_id instance,
                                       std::string_view name,
                                       const method_filter &accepts) const
    -> const method_entry * {
  const auto it = tables_->declared.find(entry.decl);
  if (it == tables_->declared.end()) {
    return nullptr;
  }
  // Inherent/impl-provided methods take priority over trait defaults,
  // which take priority over `extend`-block methods (the least specific).
  // Within the first group, an impl whose target names this exact
  // instantiation beats one that merely covers it generically.
  const method_entry *generic_match = nullptr;
  const method_entry *trait_fallback = nullptr;
  const method_entry *extension_fallback = nullptr;
  for (const auto &method : it->second) {
    if (method.decl->name != name) {
      continue;
    }
    if (accepts && !accepts(method)) {
      continue;
    }
    if (method.is_extension) {
      if (extension_fallback == nullptr) {
        extension_fallback = &method;
      }
      continue;
    }
    const auto applies =
        types_->is_unknown(instance) ||
        target_pattern_matches(method.impl_target_pattern, instance);
    if (!applies) {
      continue;
    }
    if (method.from_trait == nullptr) {
      if (!mentions_param(method.impl_target_pattern)) {
        return &method;
      }
      if (generic_match == nullptr) {
        generic_match = &method;
      }
      continue;
    }
    if (trait_fallback == nullptr) {
      trait_fallback = &method;
    }
  }
  if (generic_match != nullptr) {
    return generic_match;
  }
  return trait_fallback != nullptr ? trait_fallback : extension_fallback;
}

/// An impl-written method wins over a monomorphized trait default, the same
/// priority rule as `resolve_declared`.
auto method_resolver::resolve_builtin_impl(const type_entry &entry,
                                           std::string_view name) const
    -> const method_entry * {
  return constructor_method(entry.name, name);
}

auto method_resolver::constructor_method(std::string_view constructor,
                                         std::string_view name) const
    -> const method_entry * {
  const auto it = tables_->impl_by_builtin.find(std::string(constructor));
  if (it == tables_->impl_by_builtin.end()) {
    return nullptr;
  }
  const method_entry *trait_fallback = nullptr;
  for (const auto &method : it->second) {
    if (method.decl->name != name) {
      continue;
    }
    if (method.from_trait == nullptr) {
      return &method;
    }
    if (trait_fallback == nullptr) {
      trait_fallback = &method;
    }
  }
  return trait_fallback;
}

auto method_resolver::resolve_builtin_extend(const type_entry &entry,
                                             std::string_view name) const
    -> const method_entry * {
  const auto it = tables_->extend_by_builtin.find(entry.name);
  if (it == tables_->extend_by_builtin.end()) {
    return nullptr;
  }
  for (const auto &method : it->second) {
    if (method.decl->name == name) {
      return &method;
    }
  }
  return nullptr;
}

} // namespace cinder::semantic::detail
