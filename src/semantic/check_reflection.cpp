#include "checker.h"

// Compile-time reflection on a concrete type or module: `T.fields()`,
// `T.kind()`, `M.functions()`, ... Each call has a real type (a
// `std.traits` descriptor struct, a list of them, `type_kind`), and on a
// concrete type its answer is a constant. The checker computes that constant
// and records it (`lowering_results::reflection_folds`); lowering emits it in
// place of the call, so compile-time code that reflects runs on the VM like
// any other code.

namespace cinder::semantic::detail {

auto checker::std_traits_type(std::string_view name) -> type_id {
  if (const auto found =
          find_type_decl_by_path({"std", "traits", std::string(name)})) {
    return session_.types_.user_type(*found->first, found->second, {});
  }
  return k_unknown_type;
}

auto checker::reflection_list_type(std::string_view element) -> type_id {
  const auto element_type = element == "str" ? session_.types_.builtin("str")
                                             : std_traits_type(element);
  return element_type == k_unknown_type ? k_unknown_type
                                        : resolve_list_type(element_type);
}

auto checker::type_reflection_type(std::string_view method) -> type_id {
  if (method == "name") {
    return session_.types_.builtin("str");
  }
  if (method == "field_count" || method == "variant_count") {
    return session_.types_.builtin("int32");
  }
  if (method == "kind") {
    return std_traits_type("type_kind");
  }
  if (method == "fields") {
    return reflection_list_type("field_info");
  }
  if (method == "variants") {
    return reflection_list_type("variant_info");
  }
  return k_unknown_type;
}

auto checker::reflect_type_call(const ast::call_expr &call,
                                std::string_view method, type_id reflected)
    -> type_id {
  const auto result = type_reflection_type(method);
  if (mentions_template_param(reflected)) {
    // Each instance knows its type; the template is never lowered.
    defer_to_instances(call,
                       [this, &call, method = std::string(method),
                        reflected](instance_subst &subst) -> void {
                         fold_type_reflection(
                             *clone_of(subst, &call), method,
                             substitute_type(reflected, subst));
                       });
  } else {
    fold_type_reflection(call, method, reflected);
  }
  return result;
}

auto checker::fold_type_reflection(const ast::call_expr &call,
                                   std::string_view method, type_id reflected)
    -> void {
  const auto result = type_reflection_type(method);
  if (result == k_unknown_type) {
    return;
  }
  const auto type = strip_refs(settle(reflected));
  const auto entry = session_.types_.entry(type); // copy: types are interned
  const auto shape_error = [&](std::string_view shape) -> void {
    error_with_help(
        call.span,
        std::format("`{}()` describes a {} type, and `{}` is not one", method,
                    shape, session_.types_.display(type)),
        std::format("`{}` is not a {} type", session_.types_.display(type),
                    shape),
        method == "fields" || method == "field_count"
            ? "`fields()` and `field_count()` describe the fields of a struct "
              "type. Use `variants()` for a sum type."
            : "`variants()` and `variant_count()` describe the variants of a "
              "sum type. Use `fields()` for a struct type.");
  };
  auto value = comptime::value{};
  if (method == "name") {
    value = comptime::value::make_string(entry.name);
  } else if (method == "kind") {
    const auto *kind = "scalar_kind";
    switch (entry.kind) {
    case type_kind::struct_kind:
      kind = "struct_kind";
      break;
    case type_kind::sum_kind:
      kind = "sum_kind";
      break;
    case type_kind::array_kind:
      kind = "array_kind";
      break;
    case type_kind::fn_kind:
      kind = "fn_kind";
      break;
    default:
      break;
    }
    value = comptime::value::make_variant("type_kind", kind, {});
  } else if (method == "fields" || method == "field_count") {
    const auto *fields = struct_fields_of(entry);
    if (fields == nullptr) {
      shape_error("struct");
      return;
    }
    if (method == "field_count") {
      value = comptime::value::make_int(static_cast<int64_t>(fields->size()));
    } else {
      auto elements = std::vector<comptime::value>{};
      for (const auto &field : *fields) {
        const auto field_type = struct_field_type(entry, field.name);
        auto described = std::unordered_map<std::string, comptime::value>{};
        described.emplace("name", comptime::value::make_string(field.name));
        described.emplace("type_name",
                          comptime::value::make_string(
                              field_type.has_value()
                                  ? session_.types_.display(*field_type)
                                  : std::string("<type>")));
        described.emplace("is_data_member", comptime::value::make_bool(true));
        elements.push_back(
            comptime::value::make_struct("field_info", std::move(described)));
      }
      value = comptime::value::make_list(std::move(elements));
    }
  } else if (method == "variants" || method == "variant_count") {
    const auto *variants = sum_variants_of(entry);
    if (variants == nullptr) {
      shape_error("sum");
      return;
    }
    if (method == "variant_count") {
      value = comptime::value::make_int(static_cast<int64_t>(variants->size()));
    } else {
      auto elements = std::vector<comptime::value>{};
      for (const auto &variant : *variants) {
        auto described = std::unordered_map<std::string, comptime::value>{};
        described.emplace("name", comptime::value::make_string(variant.name));
        described.emplace("payload_count",
                          comptime::value::make_int(static_cast<int64_t>(
                              variant.payload_types.size())));
        elements.push_back(
            comptime::value::make_struct("variant_info", std::move(described)));
      }
      value = comptime::value::make_list(std::move(elements));
    }
  } else {
    return;
  }
  record_reflection_fold(call, value, result);
}

auto checker::reflect_module_call(const ast::call_expr &call,
                                  std::string_view method,
                                  std::string_view module) -> type_id {
  auto result = k_unknown_type;
  if (method == "name") {
    result = session_.types_.builtin("str");
  } else if (method == "function_count" || method == "type_count") {
    result = session_.types_.builtin("int32");
  } else if (method == "functions" || method == "types") {
    result = reflection_list_type("member_info");
  }
  if (result == k_unknown_type) {
    return result;
  }
  // The module the name means: one named exactly that, else one whose last
  // segment it is (`math` for `sample.math`).
  const module_members *members = nullptr;
  for (const auto &[name, candidate] : index_.modules) {
    if (name == module) {
      members = &candidate;
      break;
    }
    if (members == nullptr && split_module_name(name).back() == module) {
      members = &candidate;
    }
  }
  if (members == nullptr) {
    return result;
  }
  auto value = comptime::value{};
  if (method == "name") {
    value = comptime::value::make_string(std::string(module));
  } else if (method == "function_count") {
    value = comptime::value::make_int(
        static_cast<int64_t>(members->functions.size()));
  } else if (method == "type_count") {
    value =
        comptime::value::make_int(static_cast<int64_t>(members->types.size()));
  } else {
    // By name, so the list does not depend on how the module's tables
    // happen to be ordered.
    auto described = std::vector<std::pair<std::string, bool>>{};
    const auto add = [&](const auto &table) -> void {
      for (const auto &[name, ref] : table) {
        described.emplace_back(name,
                               ref.decl != nullptr && ref.decl->visibility ==
                                                          ast::visibility::pub);
      }
    };
    if (method == "functions") {
      add(members->functions);
    } else {
      add(members->types);
    }
    std::ranges::sort(described);
    auto elements = std::vector<comptime::value>{};
    for (const auto &[name, is_pub] : described) {
      auto fields = std::unordered_map<std::string, comptime::value>{};
      fields.emplace("name", comptime::value::make_string(name));
      fields.emplace("is_pub", comptime::value::make_bool(is_pub));
      elements.push_back(
          comptime::value::make_struct("member_info", std::move(fields)));
    }
    value = comptime::value::make_list(std::move(elements));
  }
  record_reflection_fold(call, value, result);
  return result;
}

auto checker::record_reflection_fold(const ast::call_expr &call,
                                     const comptime::value &value, type_id type)
    -> void {
  if (auto reified = reify_value(value, type, call.span)) {
    session_.reflection_folds_.insert_or_assign(&call, *std::move(reified));
  }
}

} // namespace cinder::semantic::detail
