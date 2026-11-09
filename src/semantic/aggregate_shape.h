#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "src/semantic/type_table.h"

namespace cinder::semantic {

// ==========================================================================
//  Concrete member types of struct and sum types.
//
//  A struct declaration spells its fields in terms of the declaration's own
//  generic parameters, and a sum declaration does the same for its payloads.
//  Layout needs the types of one *instance* (`pair[int32, str]`), with every
//  parameter substituted and every alias, qualified path, and refinement
//  resolved. The checker is the only pass that can do that substitution, so it
//  records the result per interned type and hands it on in `checked_types`.
// ==========================================================================

/// One struct field of a concrete instance: its name and substituted type.
struct field_shape {
  std::string name;
  type_id type = k_unknown_type;
};

/// One sum variant of a concrete instance: its name and payload types, in
/// declaration order. A unit variant has an empty payload.
struct variant_shape {
  std::string name;
  std::vector<type_id> payload;
};

/// The members of one struct or sum instance.
struct aggregate_shape {
  /// Struct instances only: the declaration carries the `packed` modifier.
  bool packed = false;
  /// Struct instances only: fields in declaration order.
  std::vector<field_shape> fields;
  /// Sum instances (user sums, `option`, `result`) only: variants in
  /// declaration order. The index of a variant is its tag.
  std::vector<variant_shape> variants;
};

/// The shape of every settled, closed struct and sum instance the checker
/// interned, keyed by its `type_id`. A type that still mentions a generic
/// parameter or an inference variable has no entry.
using aggregate_shapes = std::unordered_map<type_id, aggregate_shape>;

} // namespace cinder::semantic
