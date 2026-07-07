#include "src/runtime/layout.h"

#include <algorithm>
#include <cstdint>

#include "src/parser/ast.h"
#include "src/runtime/arena.h"

namespace kira::runtime {

namespace {

using semantic::type_entry;
using semantic::type_kind;
using semantic::type_table;

[[nodiscard]] auto struct_fields_of(const type_entry &instance)
    -> const std::vector<ast::struct_field> * {
  if (instance.kind != type_kind::struct_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::struct_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::struct_type_def &>(*instance.decl->definition)
              .body.fields;
}

[[nodiscard]] auto sum_variants_of(const type_entry &instance)
    -> const std::vector<ast::sum_variant> * {
  if (instance.kind != type_kind::sum_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::sum_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::sum_type_def &>(*instance.decl->definition)
              .body.variants;
}

} // namespace

auto struct_field_names(const type_table &types, semantic::type_id id)
    -> std::vector<std::string_view> {
  auto result = std::vector<std::string_view>{};
  const auto *fields = struct_fields_of(types.entry(id));
  if (fields == nullptr) {
    return result;
  }
  result.reserve(fields->size());
  for (const auto &field : *fields) {
    result.push_back(field.name);
  }
  return result;
}

auto struct_field_slot(const type_table &types, semantic::type_id id,
                       std::string_view name) -> std::optional<size_t> {
  const auto *fields = struct_fields_of(types.entry(id));
  if (fields == nullptr) {
    return std::nullopt;
  }
  for (size_t i = 0; i < fields->size(); ++i) {
    if ((*fields)[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

auto sum_variant_names(const type_table &types, semantic::type_id id)
    -> std::vector<std::string_view> {
  auto result = std::vector<std::string_view>{};
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return result;
  }
  result.reserve(variants->size());
  for (const auto &variant : *variants) {
    result.push_back(variant.name);
  }
  return result;
}

auto sum_variant_tag(const type_table &types, semantic::type_id id,
                     std::string_view name) -> std::optional<int64_t> {
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return std::nullopt;
  }
  for (size_t i = 0; i < variants->size(); ++i) {
    if ((*variants)[i].name == name) {
      return static_cast<int64_t>(i);
    }
  }
  return std::nullopt;
}

auto sum_variant_payload_slots(const type_table &types, semantic::type_id id,
                               std::string_view name) -> std::optional<size_t> {
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return std::nullopt;
  }
  for (const auto &variant : *variants) {
    if (variant.name == name) {
      return variant.payload_types.size();
    }
  }
  return std::nullopt;
}

auto sum_max_payload_slots(const type_table &types, semantic::type_id id)
    -> size_t {
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return 0;
  }
  auto max_slots = size_t{0};
  for (const auto &variant : *variants) {
    if (variant.payload_types.size() > max_slots) {
      max_slots = variant.payload_types.size();
    }
  }
  return max_slots;
}

auto list_reserve_slot(uint64_t *header) -> uint64_t * {
  const auto len = header[0];
  auto cap = header[1];
  if (len >= cap) {
    const auto new_cap = cap == 0 ? uint64_t{4} : cap * 2;
    auto *new_data = static_cast<uint64_t *>(
        global_arena().allocate(new_cap * sizeof(uint64_t)));
    if (cap > 0) {
      auto *old_data =
          reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(header[2]));
      std::copy(old_data, old_data + len, new_data);
    }
    header[1] = new_cap;
    header[2] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(new_data));
  }
  auto *data = reinterpret_cast<uint64_t *>(static_cast<uintptr_t>(header[2]));
  header[0] = len + 1;
  return data + len;
}

extern "C" auto kira_rt_list_reserve_slot(uint64_t *header) -> uint64_t * {
  return list_reserve_slot(header);
}

} // namespace kira::runtime
