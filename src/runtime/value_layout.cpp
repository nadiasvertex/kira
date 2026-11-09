#include "src/runtime/value_layout.h"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

#include "src/runtime/layout.h"

namespace cinder::runtime {

namespace {

using semantic::type_id;
using semantic::type_kind;

/// `a + b`, or `nullopt` when the sum exceeds `k_max_layout_bytes` (which also
/// covers wrapping, since the maximum is far below `SIZE_MAX`).
[[nodiscard]] auto checked_add(size_t a, size_t b) -> std::optional<size_t> {
  if (a > k_max_layout_bytes || b > k_max_layout_bytes) {
    return std::nullopt;
  }
  const auto sum = a + b;
  return sum <= k_max_layout_bytes ? std::optional{sum} : std::nullopt;
}

/// `a * b`, or `nullopt` when the product exceeds `k_max_layout_bytes`.
[[nodiscard]] auto checked_mul(size_t a, size_t b) -> std::optional<size_t> {
  if (a == 0 || b == 0) {
    return size_t{0};
  }
  if (a > k_max_layout_bytes / b) {
    return std::nullopt;
  }
  return a * b;
}

/// `offset` rounded up to a multiple of `align` (a power of two), or `nullopt`
/// on overflow.
[[nodiscard]] auto checked_round_up(size_t offset, size_t align)
    -> std::optional<size_t> {
  const auto padded = checked_add(offset, align - 1);
  if (!padded.has_value()) {
    return std::nullopt;
  }
  return *padded / align * align;
}

/// A layout with no members: a scalar, address, descriptor, closure, or handle.
[[nodiscard]] auto simple_layout(type_id id, value_layout_kind kind,
                                 size_t size, size_t align) -> value_layout {
  auto out = value_layout{};
  out.type = id;
  out.kind = kind;
  out.size = size;
  out.align = align;
  return out;
}

} // namespace

auto value_layout::find_field(std::string_view name) const
    -> const member_layout * {
  const auto found = std::ranges::find(members, name, &member_layout::name);
  return found == members.end() ? nullptr : &*found;
}

auto value_layout::find_variant(std::string_view name) const
    -> const variant_layout * {
  const auto found = std::ranges::find(variants, name, &variant_layout::name);
  return found == variants.end() ? nullptr : &*found;
}

auto value_layouts::fail(layout_error_kind kind, type_id id,
                         std::string_view reason) const -> layout_error {
  auto message =
      std::format("`{}` has no byte layout: {}", types_.display(id), reason);
  if (!path_.empty()) {
    message += " (reached through ";
    for (size_t i = 0; i < path_.size(); ++i) {
      message += std::format("{}`{}.{}`", i == 0 ? "" : ", ",
                             types_.display(path_[i].container),
                             path_[i].member);
    }
    message += ')';
  }
  return layout_error{.kind = kind, .message = std::move(message)};
}

auto value_layouts::cycle_error(type_id id) const -> layout_error {
  auto first = size_t{0};
  while (first < path_.size() && path_[first].container != id) {
    ++first;
  }
  auto message = std::format(
      "`{}` contains itself, so it has no finite size: ", types_.display(id));
  for (auto i = first; i < path_.size(); ++i) {
    message += std::format("{}`{}.{}` is `{}`", i == first ? "" : ", ",
                           types_.display(path_[i].container), path_[i].member,
                           types_.display(path_[i].type));
  }
  message += ". Put the recursive part behind a pointer, reference, or list.";
  return layout_error{.kind = layout_error_kind::infinite_size,
                      .message = std::move(message)};
}

auto value_layouts::size_of(type_id id) -> std::expected<size_t, layout_error> {
  const auto found = layout(id);
  if (!found.has_value()) {
    return std::unexpected(found.error());
  }
  return (*found)->size;
}

auto value_layouts::align_of(type_id id)
    -> std::expected<size_t, layout_error> {
  const auto found = layout(id);
  if (!found.has_value()) {
    return std::unexpected(found.error());
  }
  return (*found)->align;
}

auto value_layouts::layout(type_id id)
    -> std::expected<const value_layout *, layout_error> {
  // A refinement is its base at runtime: the predicate is a compile-time fact.
  id = types_.strip_refinement(id);
  if (const auto done = done_.find(id); done != done_.end()) {
    return &done->second;
  }
  if (const auto failure = failed_.find(id); failure != failed_.end()) {
    return std::unexpected(failure->second);
  }
  if (in_progress_.contains(id)) {
    return std::unexpected(cycle_error(id));
  }
  in_progress_.insert(id);
  auto computed = compute(id);
  in_progress_.erase(id);
  if (!computed.has_value()) {
    failed_.emplace(id, computed.error());
    return std::unexpected(std::move(computed.error()));
  }
  const auto stored = done_.emplace(id, *std::move(computed));
  return &stored.first->second;
}

auto value_layouts::member_of(type_id container, std::string name,
                              type_id type)
    -> std::expected<const value_layout *, layout_error> {
  path_.push_back(
      path_step{.container = container, .member = std::move(name), .type = type});
  auto result = layout(type);
  path_.pop_back();
  return result;
}

auto value_layouts::compute(type_id id)
    -> std::expected<value_layout, layout_error> {
  const auto &entry = types_.entry(id);
  const auto too_large = [&](std::string_view what) -> layout_error {
    return fail(layout_error_kind::too_large, id,
                std::format("{} is larger than the supported maximum of {} "
                            "bytes",
                            what, k_max_layout_bytes));
  };

  // Places `members` one after another into `out`. `packed` members are
  // adjacent and unaligned; otherwise each is aligned to its own alignment
  // and the whole is rounded up to the widest one.
  const auto place =
      [&](value_layout &out,
          const std::vector<std::pair<std::string, type_id>> &members,
          bool packed) -> std::expected<void, layout_error> {
    auto offset = size_t{0};
    auto widest = size_t{1};
    for (size_t i = 0; i < members.size(); ++i) {
      const auto &[name, type] = members[i];
      const auto label = name.empty() ? std::to_string(i) : name;
      const auto member = member_of(id, label, type);
      if (!member.has_value()) {
        return std::unexpected(member.error());
      }
      const auto &placed = **member;
      auto start = offset;
      if (!packed) {
        const auto aligned = checked_round_up(offset, placed.align);
        if (!aligned.has_value()) {
          return std::unexpected(too_large("this type"));
        }
        start = *aligned;
        widest = std::max(widest, placed.align);
      }
      const auto end = checked_add(start, placed.size);
      if (!end.has_value()) {
        return std::unexpected(too_large("this type"));
      }
      out.members.push_back(member_layout{.name = name,
                                          .type = type,
                                          .offset = start,
                                          .size = placed.size,
                                          .align = placed.align});
      offset = *end;
    }
    const auto size = packed ? std::optional{offset}
                             : checked_round_up(offset, widest);
    if (!size.has_value()) {
      return std::unexpected(too_large("this type"));
    }
    out.size = *size;
    out.align = packed ? size_t{1} : widest;
    out.packed = packed;
    return {};
  };

  // `array[T, N]` and `uninit[T, N]`: `count` contiguous elements.
  const auto place_array = [&](type_id element, uint64_t count)
      -> std::expected<value_layout, layout_error> {
    const auto member = member_of(id, "[]", element);
    if (!member.has_value()) {
      return std::unexpected(member.error());
    }
    const auto &placed = **member;
    // Zero-sized elements take no space however many there are, so the count
    // only matters for a non-empty element.
    const auto total =
        placed.size == 0 ? std::optional{size_t{0}}
        : count > k_max_layout_bytes
            ? std::nullopt
            : checked_mul(placed.size, static_cast<size_t>(count));
    if (!total.has_value()) {
      return std::unexpected(too_large("this array"));
    }
    auto out = simple_layout(id, value_layout_kind::array, *total, placed.align);
    out.element = element;
    out.stride = placed.size;
    out.count = count;
    return out;
  };

  // A sum: tag, then the widest variant payload, each laid out like a tuple.
  const auto place_sum = [&](const std::vector<semantic::variant_shape> &variants)
      -> std::expected<value_layout, layout_error> {
    auto out = simple_layout(id, value_layout_kind::sum, 0, k_sum_tag_size);
    auto widest_payload_align = size_t{1};
    auto widest_payload_size = size_t{0};
    // Payload offsets are relative to the payload area until it is placed.
    for (size_t v = 0; v < variants.size(); ++v) {
      auto variant = variant_layout{};
      variant.name = variants[v].name;
      variant.tag = static_cast<int64_t>(v);
      auto offset = size_t{0};
      auto align = size_t{1};
      for (size_t i = 0; i < variants[v].payload.size(); ++i) {
        const auto type = variants[v].payload[i];
        const auto member =
            member_of(id, std::format("{}.{}", variants[v].name, i), type);
        if (!member.has_value()) {
          return std::unexpected(member.error());
        }
        const auto &placed = **member;
        const auto aligned = checked_round_up(offset, placed.align);
        const auto end = aligned.has_value()
                             ? checked_add(*aligned, placed.size)
                             : std::nullopt;
        if (!end.has_value()) {
          return std::unexpected(too_large("this type"));
        }
        variant.payload.push_back(member_layout{.name = {},
                                                .type = type,
                                                .offset = *aligned,
                                                .size = placed.size,
                                                .align = placed.align});
        offset = *end;
        align = std::max(align, placed.align);
      }
      const auto rounded = checked_round_up(offset, align);
      if (!rounded.has_value()) {
        return std::unexpected(too_large("this type"));
      }
      widest_payload_size = std::max(widest_payload_size, *rounded);
      widest_payload_align = std::max(widest_payload_align, align);
      out.variants.push_back(std::move(variant));
    }
    const auto payload_offset =
        checked_round_up(k_sum_tag_size, widest_payload_align);
    const auto used = payload_offset.has_value()
                          ? checked_add(*payload_offset, widest_payload_size)
                          : std::nullopt;
    out.align = std::max(k_sum_tag_size, widest_payload_align);
    const auto total =
        used.has_value() ? checked_round_up(*used, out.align) : std::nullopt;
    if (!total.has_value()) {
      return std::unexpected(too_large("this type"));
    }
    out.payload_offset = *payload_offset;
    out.size = *total;
    for (auto &variant : out.variants) {
      for (auto &member : variant.payload) {
        member.offset += out.payload_offset;
      }
    }
    return out;
  };

  switch (entry.kind) {
  case type_kind::builtin_kind: {
    if (entry.name == "str") {
      return simple_layout(id, value_layout_kind::descriptor, k_descriptor_size,
                           k_word_size);
    }
    if (const auto scalar = builtin_scalar_layout(entry.name);
        scalar.has_value()) {
      return simple_layout(id, value_layout_kind::scalar, scalar->size_bytes,
                           scalar->align_bytes);
    }
    return std::unexpected(fail(layout_error_kind::unsupported, id,
                                "it has no machine representation"));
  }
  case type_kind::builtin_generic_kind: {
    if (entry.name == "slice" || entry.name == "slice_mut") {
      return simple_layout(id, value_layout_kind::descriptor, k_descriptor_size,
                           k_word_size);
    }
    if (entry.name == "cell" || entry.name == "cell_mut") {
      return simple_layout(id, value_layout_kind::address, k_word_size,
                           k_word_size);
    }
    if (entry.name == "task" || entry.name == "crew" ||
        entry.name == "generator") {
      return simple_layout(id, value_layout_kind::handle, k_word_size,
                           k_word_size);
    }
    if (entry.name == "uninit") {
      const auto count = uninit_slot_count(types_, entry);
      if (!count.has_value() || entry.args.empty()) {
        return std::unexpected(fail(layout_error_kind::unresolved, id,
                                    "its slot count is not a constant"));
      }
      return place_array(entry.args[0], *count);
    }
    if (entry.name == "option" || entry.name == "result") {
      const auto shape = shapes_.find(id);
      if (shape == shapes_.end()) {
        return std::unexpected(fail(layout_error_kind::unresolved, id,
                                    "the checker recorded no variants for it"));
      }
      return place_sum(shape->second.variants);
    }
    return std::unexpected(
        fail(layout_error_kind::unsupported, id,
             std::format("`{}` is a reserved name with no representation yet",
                         entry.name)));
  }
  case type_kind::tuple_kind: {
    auto members = std::vector<std::pair<std::string, type_id>>{};
    for (const auto element : entry.args) {
      members.emplace_back(std::string{}, element);
    }
    auto out = simple_layout(id, value_layout_kind::tuple, 0, 1);
    if (auto placed = place(out, members, /*packed=*/false); !placed) {
      return std::unexpected(std::move(placed.error()));
    }
    return out;
  }
  case type_kind::array_kind: {
    if (!entry.array_size.has_value()) {
      return std::unexpected(fail(layout_error_kind::unresolved, id,
                                  "its length is not a constant"));
    }
    return place_array(entry.result, *entry.array_size);
  }
  case type_kind::fn_kind:
    return simple_layout(id, value_layout_kind::closure, k_closure_size,
                         k_word_size);
  case type_kind::ref_kind:
  case type_kind::ptr_kind:
    return simple_layout(id, value_layout_kind::address, k_word_size,
                         k_word_size);
  case type_kind::struct_kind: {
    const auto shape = shapes_.find(id);
    if (shape == shapes_.end()) {
      return std::unexpected(fail(layout_error_kind::unresolved, id,
                                  "the checker recorded no fields for it"));
    }
    auto members = std::vector<std::pair<std::string, type_id>>{};
    for (const auto &field : shape->second.fields) {
      members.emplace_back(field.name, field.type);
    }
    auto out = simple_layout(id, value_layout_kind::structure, 0, 1);
    if (auto placed = place(out, members, shape->second.packed); !placed) {
      return std::unexpected(std::move(placed.error()));
    }
    return out;
  }
  case type_kind::sum_kind: {
    const auto shape = shapes_.find(id);
    if (shape == shapes_.end()) {
      return std::unexpected(fail(layout_error_kind::unresolved, id,
                                  "the checker recorded no variants for it"));
    }
    return place_sum(shape->second.variants);
  }
  case type_kind::opaque_kind:
    return std::unexpected(
        fail(layout_error_kind::unsupported, id,
             "an alias or opaque type has no layout of its own; it must be "
             "resolved to the type it stands for"));
  case type_kind::existential_kind:
    return std::unexpected(fail(layout_error_kind::unsupported, id,
                                "an opaque `some` type must be resolved to its "
                                "concrete type"));
  case type_kind::const_value_kind:
  case type_kind::symbolic_value_kind:
  case type_kind::const_variant_kind:
    return std::unexpected(fail(layout_error_kind::unsupported, id,
                                "it is a compile-time value, not a type"));
  case type_kind::unknown_kind:
  case type_kind::error_kind:
  case type_kind::type_param_kind:
  case type_kind::ctor_ref_kind:
  case type_kind::param_app_kind:
  case type_kind::type_var_kind:
  case type_kind::refinement_kind:
    break;
  }
  return std::unexpected(fail(layout_error_kind::unresolved, id,
                              "the type is not settled to a concrete type"));
}

} // namespace cinder::runtime
