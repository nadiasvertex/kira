#include "src/bytecode/value.h"

#include "src/semantic/types.h"

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
  const auto &name = entry.name;
  if (name == "bool") {
    return numeric_kind::boolean;
  }
  if (name == "char") {
    return numeric_kind::character;
  }
  if (name == "int8") {
    return numeric_kind::i8;
  }
  if (name == "int16") {
    return numeric_kind::i16;
  }
  if (name == "int32") {
    return numeric_kind::i32;
  }
  if (name == "int64" || name == "isize") {
    return numeric_kind::i64;
  }
  if (name == "uint8" || name == "byte") {
    return numeric_kind::u8;
  }
  if (name == "uint16") {
    return numeric_kind::u16;
  }
  if (name == "uint32") {
    return numeric_kind::u32;
  }
  if (name == "uint64" || name == "usize") {
    return numeric_kind::u64;
  }
  if (name == "float32") {
    return numeric_kind::f32;
  }
  if (name == "float64") {
    return numeric_kind::f64;
  }
  // int128/uint128/float128 and every non-scalar builtin (str, unit, never,
  // ordering, io, cpu, ...) fall through to `nullopt` — see the doc comment
  // on `numeric_kind_of` in value.h for why 128-bit widths aren't supported
  // by this increment, and non-scalars simply aren't numeric.
  return std::nullopt;
}

} // namespace kira::bytecode
