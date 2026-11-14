#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/semantic/aggregate_shape.h"
#include "src/semantic/type_table.h"

namespace cinder::runtime {

// ==========================================================================
//  Inline value layouts (representation v2).
//
//  Every value owns its bytes in place: a struct, tuple, fixed array, or sum
//  is the concatenation of its members, with explicit offsets and padding.
//  Both backends, the storage form, and compile-time value conversion read
//  their layouts from here (`spec/value-semantics.md`).
//
//  | Type family                | Bytes                                   |
//  |----------------------------|-----------------------------------------|
//  | scalar                     | natural size and alignment; `unit` is 0 |
//  | `&T`, `*T`, `cell[T]`      | one 8-byte address                      |
//  | `str`, `slice[T]`          | `{len: u64, data: address}`, 16 bytes   |
//  | `fn` value                 | `{entry, env, drop_glue}`, 24 bytes     |
//  | `task`, `crew`, `generator`| one 8-byte handle to runtime state      |
//  | struct, tuple              | members in order, padded (or packed)    |
//  | `array[T, N]`, `uninit`    | `N` contiguous `T`, stride `size_of(T)` |
//  | sum, `option`, `result`    | `u64` tag, then the widest payload      |
//
//  A sum places its payload at the tag's end rounded up to the widest
//  variant alignment and rounds the whole value up to the larger of 8 and
//  that alignment. Each variant's payload is laid out like a tuple. The tag
//  is the variant's index in declaration order.
//
//  A type that names itself by value has no finite size and is rejected with
//  the chain of members that closes the cycle. A cycle through an address, a
//  handle, or a library collection (whose own fields hold a pointer) is
//  finite and accepted.
// ==========================================================================

/// Which family of representation a `value_layout` describes.
enum class value_layout_kind : uint8_t {
  scalar,     ///< A builtin scalar, including zero-sized `unit`.
  address,    ///< `&T`, `&mut T`, `*T`, `cell[T]`, `cell_mut[T]`.
  descriptor, ///< `str`, `slice[T]`, `slice_mut[T]`: `{len, data}`.
  closure,    ///< A `fn` value: `{entry, env, drop_glue}`.
  handle,     ///< `task`, `crew`, `generator`: one handle word.
  structure,  ///< A struct, with `members`.
  tuple,      ///< A tuple, with `members`.
  array,      ///< `array[T, N]` or `uninit[T, N]`.
  sum,        ///< A sum, `option`, or `result`, with `variants`.
};

/// Byte offset of the length word in a `descriptor`.
inline constexpr size_t k_descriptor_len_offset = 0;
/// Byte offset of the data address in a `descriptor`.
inline constexpr size_t k_descriptor_data_offset = 8;
/// Size of a `descriptor`.
inline constexpr size_t k_descriptor_size = 16;
/// Size of a `closure` value.
inline constexpr size_t k_closure_size = 24;
/// Size and alignment of an `address` or a `handle`.
inline constexpr size_t k_word_size = 8;
/// Size and alignment of a sum's tag.
inline constexpr size_t k_sum_tag_size = 8;
/// The largest size or offset a layout may have. Anything larger is reported
/// as `too_large` instead of wrapping.
inline constexpr size_t k_max_layout_bytes =
    static_cast<size_t>(INT64_MAX) / 2;

/// One placed member: a struct field, tuple element, or variant payload slot.
struct member_layout {
  std::string name; ///< Field name; empty for a tuple element or payload.
  semantic::type_id type = semantic::k_unknown_type;
  size_t offset = 0; ///< Bytes from the start of the enclosing value.
  size_t size = 0;
  size_t align = 1;
};

/// One placed sum variant.
struct variant_layout {
  std::string name;
  int64_t tag = 0;
  /// Payload members, in order; `offset` is from the start of the sum.
  std::vector<member_layout> payload;
};

/// The complete layout of one concrete type.
struct value_layout {
  semantic::type_id type = semantic::k_unknown_type;
  value_layout_kind kind = value_layout_kind::scalar;
  size_t size = 0;
  size_t align = 1;
  /// Struct only: the declaration is `packed`, so members are unaligned.
  bool packed = false;
  /// Struct fields or tuple elements, in order.
  std::vector<member_layout> members;
  /// Array only: element type, byte distance between elements, and count.
  semantic::type_id element = semantic::k_unknown_type;
  size_t stride = 0;
  uint64_t count = 0;
  /// Sum only: the tag is at `tag_offset`; the payload area starts at
  /// `payload_offset` and every variant's payload offsets include it.
  size_t tag_offset = 0;
  size_t payload_offset = 0;
  std::vector<variant_layout> variants;

  /// The struct field named `name`, or null.
  [[nodiscard]] auto find_field(std::string_view name) const
      -> const member_layout *;
  /// The sum variant named `name`, or null.
  [[nodiscard]] auto find_variant(std::string_view name) const
      -> const variant_layout *;
  /// The size and alignment of one zero-sized value need no storage; a
  /// reference to one still needs an aligned address.
  [[nodiscard]] auto is_zero_sized() const -> bool { return size == 0; }
};

/// Why a type has no layout.
enum class layout_error_kind : uint8_t {
  /// The type has no byte layout: an alias that was never resolved, a reserved
  /// builtin with no representation, `float128`.
  unsupported,
  /// The type contains itself by value.
  infinite_size,
  /// A size or offset exceeds `k_max_layout_bytes`.
  too_large,
  /// The type is not settled: unknown, generic, or a member not recorded.
  unresolved,
};

/// A layout failure. `message` is written for the user and names the member
/// path that led to the failing type.
struct layout_error {
  layout_error_kind kind = layout_error_kind::unsupported;
  std::string message;
};

/// The natural size and alignment of a builtin scalar.
struct scalar_layout {
  size_t size_bytes = 0;
  size_t align_bytes = 1;
};

/// The natural size and alignment of the builtin scalar named `name`, or
/// `nullopt` when `name` is not a scalar with a byte layout (`float128`,
/// `str`, and every non-builtin name).
[[nodiscard]] auto builtin_scalar_layout(std::string_view name)
    -> std::optional<scalar_layout>;

/// The `N` of a `uninit[T, N]`, or `nullopt` when the slot count is not a
/// definite compile-time constant (an un-instantiated generic template).
/// `N` is a const-generic *value* argument, so it lives in the entry's
/// second type argument as a `const_value_kind` rather than in `array_size`
/// the way a fixed `array[T, N]`'s length does.
[[nodiscard]] auto uninit_slot_count(const semantic::type_table &types,
                                     const semantic::type_entry &entry)
    -> std::optional<uint64_t>;

/// Computes and caches the layout of concrete types.
///
/// Holds references to the type table and the shapes it was built from; both
/// must outlive it. Layouts are computed on first request, and a failure is
/// cached too, so asking again is cheap and gives the same answer. A returned
/// pointer stays valid for the life of the object.
class value_layouts {
public:
  value_layouts(const semantic::type_table &types,
                const semantic::aggregate_shapes &shapes)
      : types_(types), shapes_(shapes) {}

  /// The layout of `id`. A refinement lays out as its base type.
  [[nodiscard]] auto layout(semantic::type_id id)
      -> std::expected<const value_layout *, layout_error>;

  /// `layout(id)->size`, or the error.
  [[nodiscard]] auto size_of(semantic::type_id id)
      -> std::expected<size_t, layout_error>;
  /// `layout(id)->align`, or the error.
  [[nodiscard]] auto align_of(semantic::type_id id)
      -> std::expected<size_t, layout_error>;

private:
  /// One step of the path from the type being asked about to the type being
  /// laid out now: the container and the member that leads to the next type.
  struct path_step {
    semantic::type_id container = semantic::k_unknown_type;
    std::string member;
    semantic::type_id type = semantic::k_unknown_type; ///< The member's type.
  };

  [[nodiscard]] auto compute(semantic::type_id id)
      -> std::expected<value_layout, layout_error>;
  [[nodiscard]] auto
  member_of(semantic::type_id container, std::string name,
            semantic::type_id type)
      -> std::expected<const value_layout *, layout_error>;
  [[nodiscard]] auto fail(layout_error_kind kind, semantic::type_id id,
                          std::string_view reason) const -> layout_error;
  [[nodiscard]] auto cycle_error(semantic::type_id id) const -> layout_error;

  const semantic::type_table &types_;
  const semantic::aggregate_shapes &shapes_;
  std::unordered_map<semantic::type_id, value_layout> done_;
  std::unordered_map<semantic::type_id, layout_error> failed_;
  std::unordered_set<semantic::type_id> in_progress_;
  std::vector<path_step> path_;
};

} // namespace cinder::runtime
