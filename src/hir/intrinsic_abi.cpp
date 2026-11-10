#include "src/hir/intrinsic_abi.h"

#include <format>

#include "src/intrinsics.h"

namespace cinder::hir {

namespace {

/// Whether a scalar of `size` bytes can cross as `wire`.
[[nodiscard]] auto scalar_fits(intrinsic_wire_kind wire, size_t size) -> bool {
  switch (wire) {
  case intrinsic_wire_kind::i32:
  case intrinsic_wire_kind::f32:
    return size <= 4;
  case intrinsic_wire_kind::i64:
  case intrinsic_wire_kind::f64:
    return size == 8;
  case intrinsic_wire_kind::ptr:
    return false;
  }
  return false;
}

} // namespace

auto runtime_intrinsic_id(std::string_view name) -> std::optional<size_t> {
  const auto id = intrinsic_index_of(name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  return *id;
}

auto check_intrinsic_signature(const abi_signature &signature, size_t id)
    -> std::optional<std::string> {
  const auto name = known_intrinsic_names.at(id);
  const auto builds_into = intrinsic_builds_into(id);
  const auto through_address = signature.returns_through_address();
  if (builds_into && !through_address) {
    return std::format(
        "`{}` writes its result into caller storage, but its declared result "
        "is {} and travels in a register",
        name, pass_class_name(signature.result.cls));
  }
  if (!builds_into && through_address) {
    return std::format(
        "`{}` returns its result in a register, but its declared result is {} "
        "and is built in caller storage; give the runtime an `_into` symbol "
        "for it",
        name, pass_class_name(signature.result.cls));
  }

  if (!builds_into) {
    const auto wire = known_intrinsic_return_kinds.at(id);
    switch (signature.result.cls) {
    case pass_class::scalar:
      if (!scalar_fits(wire, signature.result.size)) {
        return std::format("the {}-byte scalar result of `{}` does not fit the "
                           "runtime symbol's return type",
                           signature.result.size, name);
      }
      break;
    case pass_class::address:
    case pass_class::handle:
      if (wire != intrinsic_wire_kind::ptr) {
        return std::format("the {} result of `{}` must come back as a "
                           "pointer, but the runtime symbol returns a number",
                           pass_class_name(signature.result.cls), name);
      }
      break;
    case pass_class::zero_sized:
    case pass_class::descriptor:
    case pass_class::aggregate:
      break;
    }
  }

  const auto arity = known_intrinsic_arities.at(id);
  auto position = size_t{0};
  for (const auto &param : signature.params) {
    if (param.cls == pass_class::zero_sized) {
      continue;
    }
    if (position >= arity) {
      return std::format("`{}` is declared with more parameters than its "
                         "runtime symbol takes ({})",
                         name, arity);
    }
    const auto wire = known_intrinsic_param_kinds.at(id).at(position);
    if (param.cls == pass_class::scalar) {
      if (!scalar_fits(wire, param.size)) {
        return std::format("parameter {} of `{}` is a {}-byte scalar that does "
                           "not fit the runtime symbol's parameter",
                           position + 1, name, param.size);
      }
    } else if (wire != intrinsic_wire_kind::ptr) {
      return std::format("parameter {} of `{}` is {} and must cross as a "
                         "pointer, but the runtime symbol takes a number",
                         position + 1, name, pass_class_name(param.cls));
    }
    ++position;
  }
  if (position != arity) {
    return std::format("`{}` is declared with {} parameters, but its runtime "
                       "symbol takes {}",
                       name, position, arity);
  }
  return std::nullopt;
}

} // namespace cinder::hir
