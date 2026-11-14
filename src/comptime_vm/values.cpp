#include "src/comptime_vm/values.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/runtime/allocator.h"
#include "src/runtime/value_layout.h"

namespace cinder::comptime_vm {

namespace {

using bytecode::numeric_kind;
using bytecode::slot_value;
using semantic::type_id;
using semantic::type_kind;

[[nodiscard]] auto pointer_of(slot_value slot) -> std::uint8_t * {
  return reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(slot.u));
}

[[nodiscard]] auto slot_of(void *pointer) -> slot_value {
  return slot_value{
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer))};
}

/// The low `size` bytes of `slot` written at `at`, the way `op_store_slot`
/// writes a field.
auto store(std::uint8_t *at, std::size_t size, slot_value slot) -> void {
  const auto raw = slot.w;
  std::memcpy(at, &raw, size);
}

/// `size` bytes at `at`, zero-extended, the way `op_load_slot` reads a field.
[[nodiscard]] auto load(const std::uint8_t *at, std::size_t size)
    -> slot_value {
  auto raw = static_cast<unsigned __int128>(0);
  std::memcpy(&raw, at, size);
  return slot_value{raw};
}

[[nodiscard]] auto allocate(std::size_t bytes) -> std::uint8_t * {
  return static_cast<std::uint8_t *>(cinder_heap_alloc(bytes));
}

using wide_unsigned = unsigned __int128;

[[nodiscard]] auto mask(int bits) -> wide_unsigned {
  return bits >= 128 ? ~wide_unsigned{0} : (wide_unsigned{1} << bits) - 1;
}

/// Whether `entry` is the standard library's `list[T]`.
[[nodiscard]] auto is_list(const semantic::type_entry &entry) -> bool {
  return entry.kind == type_kind::struct_kind && entry.name == "list" &&
         entry.module_name == "std.list" && entry.args.size() == 1;
}

[[nodiscard]] auto is_sum(const semantic::type_entry &entry) -> bool {
  return entry.kind == type_kind::sum_kind ||
         (entry.kind == type_kind::builtin_generic_kind &&
          (entry.name == "option" || entry.name == "result"));
}

class converter {
public:
  /// `added` receives the values a write puts in the handle table; it is
  /// null for a converter that only reads.
  converter(semantic::comptime_program &program, const handle_table &handles,
            handle_table *added, runtime::value_layouts *layouts = nullptr)
      : program_(program), types_(program.view().types), handles_(handles),
        added_(added), layouts_(layouts) {}

  // ------------------------------------------------------------------
  //  The storage form: every value is its inline bytes
  //  (`runtime::value_layouts`), and only the data behind an address (a
  //  `str`'s bytes, a `list`'s elements, a reference's referent) is on the
  //  heap.
  // ------------------------------------------------------------------

  /// Writes `value` as the bytes of a `type` at `at`, which has room for
  /// the type's layout.
  [[nodiscard]] auto write_bytes(type_id type, const comptime::value &value,
                                 std::uint8_t *at)
      -> std::expected<void, std::string> {
    const auto layout = layout_of(type);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &entry = types_.entry(type);
    if ((*layout)->is_zero_sized()) {
      return {};
    }
    if (is_handle_type(types_, type) ||
        bytecode::numeric_kind_of(types_, type).has_value()) {
      auto slot = word_to_slot(type, value);
      if (!slot.has_value()) {
        return std::unexpected(slot.error());
      }
      store(at, (*layout)->size, *slot);
      return {};
    }
    if (entry.kind == type_kind::builtin_kind && entry.name == "str") {
      if (value.kind != comptime::value_kind::string) {
        return mismatch(type);
      }
      auto *bytes = allocate(std::max<std::size_t>(value.string.size(), 1));
      if (!value.string.empty()) {
        std::memcpy(bytes, value.string.data(), value.string.size());
      }
      store(at + runtime::k_descriptor_len_offset, 8,
            slot_value{std::uint64_t{value.string.size()}});
      store(at + runtime::k_descriptor_data_offset, 8, slot_of(bytes));
      return {};
    }
    if (is_list(entry)) {
      return list_to_bytes(type, entry.args[0], value, **layout, at);
    }
    switch ((*layout)->kind) {
    case runtime::value_layout_kind::address: {
      if (entry.kind != type_kind::ref_kind) {
        return unsupported(type);
      }
      // A fresh place, so a callee that writes through `&mut` changes a
      // copy.
      const auto inner = layout_of(entry.result);
      if (!inner.has_value()) {
        return std::unexpected(inner.error());
      }
      auto *place = allocate(std::max<std::size_t>((*inner)->size, 1));
      if (auto ok = write_bytes(entry.result, value, place); !ok.has_value()) {
        return ok;
      }
      store(at, 8, slot_of(place));
      return {};
    }
    case runtime::value_layout_kind::structure: {
      if (value.kind != comptime::value_kind::struct_instance) {
        return mismatch(type);
      }
      for (const auto &member : (*layout)->members) {
        const auto found = value.fields.find(member.name);
        if (found == value.fields.end()) {
          return mismatch(type);
        }
        if (auto ok =
                write_bytes(member.type, found->second, at + member.offset);
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    }
    case runtime::value_layout_kind::tuple:
      if (value.kind != comptime::value_kind::list ||
          value.elements.size() != (*layout)->members.size()) {
        return mismatch(type);
      }
      for (std::size_t i = 0; i < value.elements.size(); ++i) {
        const auto &member = (*layout)->members[i];
        if (auto ok =
                write_bytes(member.type, value.elements[i], at + member.offset);
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    case runtime::value_layout_kind::array:
      if (value.kind != comptime::value_kind::list ||
          value.elements.size() != (*layout)->count) {
        return mismatch(type);
      }
      for (std::size_t i = 0; i < value.elements.size(); ++i) {
        if (auto ok = write_bytes((*layout)->element, value.elements[i],
                                  at + (i * (*layout)->stride));
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    case runtime::value_layout_kind::sum: {
      if (value.kind != comptime::value_kind::variant_instance) {
        return mismatch(type);
      }
      const auto *variant = (*layout)->find_variant(value.variant_tag);
      if (variant == nullptr ||
          variant->payload.size() != value.elements.size()) {
        return mismatch(type);
      }
      store(at + (*layout)->tag_offset, runtime::k_sum_tag_size,
            slot_value{static_cast<std::uint64_t>(variant->tag)});
      for (std::size_t i = 0; i < value.elements.size(); ++i) {
        const auto &member = variant->payload[i];
        if (auto ok =
                write_bytes(member.type, value.elements[i], at + member.offset);
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    }
    default:
      return unsupported(type);
    }
  }

  /// The `type` whose bytes are at `at`, as a compile-time value.
  [[nodiscard]] auto read_bytes(type_id type, const std::uint8_t *at)
      -> std::expected<comptime::value, std::string> {
    const auto layout = layout_of(type);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &entry = types_.entry(type);
    if (types_.is_unit(type)) {
      return comptime::value::make_unit();
    }
    if (is_handle_type(types_, type) ||
        bytecode::numeric_kind_of(types_, type).has_value()) {
      return word_from_slot(type, load(at, (*layout)->size));
    }
    if (entry.kind == type_kind::builtin_kind && entry.name == "str") {
      const auto length = load(at + runtime::k_descriptor_len_offset, 8).u;
      const auto *bytes =
          pointer_of(load(at + runtime::k_descriptor_data_offset, 8));
      return comptime::value::make_string(
          length == 0 ? std::string{}
                      : std::string(reinterpret_cast<const char *>(bytes),
                                    static_cast<std::size_t>(length)));
    }
    if (is_list(entry)) {
      return list_from_bytes(type, entry.args[0], **layout, at);
    }
    switch ((*layout)->kind) {
    case runtime::value_layout_kind::address:
      if (entry.kind != type_kind::ref_kind) {
        return unsupported(type);
      }
      return read_bytes(entry.result, pointer_of(load(at, 8)));
    case runtime::value_layout_kind::structure: {
      auto fields = std::unordered_map<std::string, comptime::value>{};
      for (const auto &member : (*layout)->members) {
        auto field = read_bytes(member.type, at + member.offset);
        if (!field.has_value()) {
          return std::unexpected(field.error());
        }
        fields.emplace(member.name, std::move(*field));
      }
      return comptime::value::make_struct(entry.name, std::move(fields));
    }
    case runtime::value_layout_kind::tuple: {
      auto values = std::vector<comptime::value>{};
      for (const auto &member : (*layout)->members) {
        auto element = read_bytes(member.type, at + member.offset);
        if (!element.has_value()) {
          return std::unexpected(element.error());
        }
        values.push_back(std::move(*element));
      }
      return comptime::value::make_list(std::move(values));
    }
    case runtime::value_layout_kind::array: {
      auto values = std::vector<comptime::value>{};
      for (std::uint64_t i = 0; i < (*layout)->count; ++i) {
        auto element =
            read_bytes((*layout)->element,
                       at + (static_cast<std::size_t>(i) * (*layout)->stride));
        if (!element.has_value()) {
          return std::unexpected(element.error());
        }
        values.push_back(std::move(*element));
      }
      return comptime::value::make_list(std::move(values));
    }
    case runtime::value_layout_kind::sum: {
      const auto tag =
          load(at + (*layout)->tag_offset, runtime::k_sum_tag_size).i;
      const auto found =
          std::ranges::find((*layout)->variants, static_cast<std::int64_t>(tag),
                            &runtime::variant_layout::tag);
      if (found == (*layout)->variants.end()) {
        return unsupported(type);
      }
      auto values = std::vector<comptime::value>{};
      for (const auto &member : found->payload) {
        auto element = read_bytes(member.type, at + member.offset);
        if (!element.has_value()) {
          return std::unexpected(element.error());
        }
        values.push_back(std::move(*element));
      }
      return comptime::value::make_variant(entry.name, found->name,
                                           std::move(values));
    }
    default:
      return unsupported(type);
    }
  }

  /// `value` as the register a scalar, address or handle of `type` travels
  /// in.
  [[nodiscard]] auto to_register(type_id type, const comptime::value &value)
      -> std::expected<slot_value, std::string> {
    if (bytecode::numeric_kind_of(types_, type).has_value() ||
        is_handle_type(types_, type) || types_.is_unit(type)) {
      return word_to_slot(type, value);
    }
    alignas(16) std::uint8_t bytes[16] = {};
    if (auto ok = write_bytes(type, value, bytes); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    return load(bytes, 16);
  }

  /// The register `slot` holding a `type`, as a compile-time value.
  [[nodiscard]] auto from_register(type_id type, slot_value slot)
      -> std::expected<comptime::value, std::string> {
    alignas(16) std::uint8_t bytes[16] = {};
    store(bytes, 16, slot);
    return read_bytes(type, bytes);
  }

  /// A unit, scalar or handle `value` as the register it travels in.
  [[nodiscard]] auto word_to_slot(type_id type, const comptime::value &value)
      -> std::expected<slot_value, std::string> {
    if (types_.is_unit(type)) {
      return slot_value{};
    }
    if (is_handle_type(types_, type)) {
      if (added_ == nullptr || value.fragment == nullptr) {
        return mismatch(type);
      }
      added_->values.push_back(value);
      return slot_value{std::uint64_t{added_->values.size()}};
    }
    if (const auto kind = bytecode::numeric_kind_of(types_, type)) {
      return scalar_to_slot(type, *kind, value);
    }
    return unsupported(type);
  }

  /// The unit, scalar or handle of `type` in register `slot`.
  [[nodiscard]] auto word_from_slot(type_id type, slot_value slot)
      -> std::expected<comptime::value, std::string> {
    if (types_.is_unit(type)) {
      return comptime::value::make_unit();
    }
    if (is_handle_type(types_, type)) {
      if (slot.u == 0 || slot.u > handles_.values.size()) {
        return std::unexpected(std::format(
            "a `{}` value is not one the compiler made", types_.display(type)));
      }
      return handles_.values[slot.u - 1];
    }
    if (const auto kind = bytecode::numeric_kind_of(types_, type)) {
      return scalar_from_slot(*kind, slot);
    }
    return unsupported(type);
  }

private:
  [[nodiscard]] auto unsupported(type_id type) const
      -> std::unexpected<std::string> {
    return std::unexpected(std::format(
        "a value of type `{}` cannot be passed between the compiler and the "
        "VM yet",
        types_.display(type)));
  }

  [[nodiscard]] auto mismatch(type_id type) const
      -> std::unexpected<std::string> {
    return std::unexpected(
        std::format("the compile-time value does not have the shape of a `{}`",
                    types_.display(type)));
  }

  [[nodiscard]] auto layout_of(type_id type) const
      -> std::expected<const runtime::value_layout *, std::string> {
    if (layouts_ == nullptr) {
      return unsupported(type);
    }
    auto layout = layouts_->layout(type);
    if (!layout.has_value()) {
      return std::unexpected(
          std::format("a value of type `{}` has no byte layout: {}",
                      types_.display(type), layout.error().message));
    }
    return *layout;
  }

  /// A `list[T]` is the library struct `{len, cap, data}`; its elements are
  /// one heap block at the element's stride, which the list owns.
  [[nodiscard]] auto
  list_to_bytes(type_id type, type_id element, const comptime::value &value,
                const runtime::value_layout &layout, std::uint8_t *at)
      -> std::expected<void, std::string> {
    if (value.kind != comptime::value_kind::list) {
      return mismatch(type);
    }
    const auto *len = layout.find_field("len");
    const auto *cap = layout.find_field("cap");
    const auto *data = layout.find_field("data");
    const auto element_layout = layout_of(element);
    if (len == nullptr || cap == nullptr || data == nullptr) {
      return unsupported(type);
    }
    if (!element_layout.has_value()) {
      return std::unexpected(element_layout.error());
    }
    const auto stride = (*element_layout)->size;
    auto *block =
        allocate(std::max<std::size_t>(stride * value.elements.size(), 1));
    for (std::size_t i = 0; i < value.elements.size(); ++i) {
      if (auto ok =
              write_bytes(element, value.elements[i], block + (i * stride));
          !ok.has_value()) {
        return ok;
      }
    }
    const auto count = slot_value{std::uint64_t{value.elements.size()}};
    store(at + len->offset, len->size, count);
    store(at + cap->offset, cap->size, count);
    store(at + data->offset, data->size, slot_of(block));
    return {};
  }

  [[nodiscard]] auto list_from_bytes(type_id type, type_id element,
                                     const runtime::value_layout &layout,
                                     const std::uint8_t *at)
      -> std::expected<comptime::value, std::string> {
    const auto *len = layout.find_field("len");
    const auto *data = layout.find_field("data");
    const auto element_layout = layout_of(element);
    if (len == nullptr || data == nullptr) {
      return unsupported(type);
    }
    if (!element_layout.has_value()) {
      return std::unexpected(element_layout.error());
    }
    const auto count = load(at + len->offset, len->size).u;
    const auto *block = pointer_of(load(at + data->offset, data->size));
    auto values = std::vector<comptime::value>{};
    for (std::uint64_t i = 0; i < count; ++i) {
      auto value = read_bytes(element, block + (static_cast<std::size_t>(i) *
                                                (*element_layout)->size));
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      values.push_back(std::move(*value));
    }
    return comptime::value::make_list(std::move(values));
  }

  [[nodiscard]] auto scalar_to_slot(type_id type, numeric_kind kind,
                                    const comptime::value &value)
      -> std::expected<slot_value, std::string> {
    if (kind == numeric_kind::boolean) {
      if (value.kind != comptime::value_kind::boolean) {
        return mismatch(type);
      }
      return slot_value{std::uint64_t{value.boolean ? 1U : 0U}};
    }
    if (bytecode::is_float(kind)) {
      auto number = value.floating;
      if (value.kind == comptime::value_kind::integer) {
        number = static_cast<double>(value.integer);
      } else if (value.kind != comptime::value_kind::floating) {
        return mismatch(type);
      }
      if (kind == numeric_kind::f32) {
        return slot_value{static_cast<std::uint64_t>(
            std::bit_cast<std::uint32_t>(static_cast<float>(number)))};
      }
      return slot_value{number};
    }
    if (value.kind != comptime::value_kind::integer) {
      return mismatch(type);
    }
    const auto bits = bytecode::bit_width(kind);
    const auto does_not_fit = [&] {
      return std::unexpected(
          std::format("the compile-time value {} does not fit in `{}`",
                      value.integer, types_.display(type)));
    };
    if (bytecode::is_signed_integer(kind)) {
      if (bits < 128) {
        const auto low = -(__int128{1} << (bits - 1));
        const auto high = (__int128{1} << (bits - 1)) - 1;
        if (value.integer < low || value.integer > high) {
          return does_not_fit();
        }
      }
      return slot_value{value.integer};
    }
    // A `uint128` past `int128`'s maximum travels as its bit pattern (see
    // `comptime::value`), so a 128-bit target takes any value as is.
    const auto raw = static_cast<wide_unsigned>(value.integer);
    if (bits < 128 && (value.integer < 0 || (raw & ~mask(bits)) != 0)) {
      return does_not_fit();
    }
    return slot_value{raw};
  }

  [[nodiscard]] static auto scalar_from_slot(numeric_kind kind, slot_value slot)
      -> comptime::value {
    if (kind == numeric_kind::boolean) {
      return comptime::value::make_bool((slot.u & 1U) != 0U);
    }
    if (kind == numeric_kind::f32) {
      return comptime::value::make_float(static_cast<double>(
          std::bit_cast<float>(static_cast<std::uint32_t>(slot.u))));
    }
    if (kind == numeric_kind::f64) {
      return comptime::value::make_float(slot.f);
    }
    const auto bits = bytecode::bit_width(kind);
    auto raw = slot.w & mask(bits);
    if (bytecode::is_signed_integer(kind) && bits < 128 &&
        (raw & (wide_unsigned{1} << (bits - 1))) != 0) {
      raw |= ~mask(bits);
    }
    return comptime::value::make_int(static_cast<__int128>(raw));
  }

  semantic::comptime_program &program_;
  const semantic::type_table &types_;
  const handle_table &handles_;
  handle_table *added_;
  runtime::value_layouts *layouts_;
};

} // namespace

auto is_handle_type(const semantic::type_table &types, semantic::type_id type)
    -> bool {
  const auto &entry = types.entry(type);
  return entry.kind == type_kind::builtin_kind &&
         (entry.name == "expr" || entry.name == "stmt" ||
          entry.name == "def_expr" || entry.name == "type_expr");
}

auto write_inline(semantic::comptime_program &program, handle_table &handles,
                  runtime::value_layouts &layouts, semantic::type_id type,
                  const comptime::value &value, std::byte *at)
    -> std::expected<void, std::string> {
  return converter(program, handles, &handles, &layouts)
      .write_bytes(type, value, reinterpret_cast<std::uint8_t *>(at));
}

auto read_inline(semantic::comptime_program &program,
                 const handle_table &handles, runtime::value_layouts &layouts,
                 semantic::type_id type, const std::byte *at)
    -> std::expected<comptime::value, std::string> {
  return converter(program, handles, nullptr, &layouts)
      .read_bytes(type, reinterpret_cast<const std::uint8_t *>(at));
}

auto to_register(semantic::comptime_program &program, handle_table &handles,
                 runtime::value_layouts &layouts, semantic::type_id type,
                 const comptime::value &value)
    -> std::expected<bytecode::slot_value, std::string> {
  return converter(program, handles, &handles, &layouts)
      .to_register(type, value);
}

auto from_register(semantic::comptime_program &program,
                   const handle_table &handles, runtime::value_layouts &layouts,
                   semantic::type_id type, bytecode::slot_value slot)
    -> std::expected<comptime::value, std::string> {
  return converter(program, handles, nullptr, &layouts)
      .from_register(type, slot);
}

} // namespace cinder::comptime_vm
