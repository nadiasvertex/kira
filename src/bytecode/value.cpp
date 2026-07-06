#include "src/bytecode/value.h"

#include "src/semantic/types.h"

#include <unordered_map>

namespace kira::bytecode {

using semantic::type_id;
using semantic::type_kind;
using semantic::type_table;

auto numeric_kind_of(const type_table &types, type_id id)
    -> std::optional<numeric_kind> {
  const auto &entry = types.entry(id);
  if (entry.kind != type_kind::builtin_kind) {
    return std::nullopt;
  }
  static const std::unordered_map<std::string_view, numeric_kind> map = {
      {"bool", numeric_kind::boolean}, {"char", numeric_kind::character},
      {"int8", numeric_kind::i8},      {"int16", numeric_kind::i16},
      {"int32", numeric_kind::i32},    {"int64", numeric_kind::i64},
      {"isize", numeric_kind::i64},    {"uint8", numeric_kind::u8},
      {"byte", numeric_kind::u8},      {"uint16", numeric_kind::u16},
      {"uint32", numeric_kind::u32},   {"uint64", numeric_kind::u64},
      {"usize", numeric_kind::u64},    {"float32", numeric_kind::f32},
      {"float64", numeric_kind::f64},
  };

  if (auto it = map.find(entry.name); it != map.end()) {
    return it->second;
  }

  // int128/uint128/float128 and every non-scalar builtin (str, unit, never,
  // ordering, io, cpu, ...) fall through to `nullopt` — see the doc comment
  // on `numeric_kind_of` in value.h for why 128-bit widths aren't supported
  // by this increment, and non-scalars simply aren't numeric.
  return std::nullopt;
}

} // namespace kira::bytecode
