#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "src/hir/nodes.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/type_table.h"

namespace cinder::hir {

// ==========================================================================
//  Aggregate ABI (representation v2).
//
//  Every value is passed or returned in one of a few ways, decided from its
//  layout alone:
//
//  | class       | values                                       | travels as |
//  |-------------|----------------------------------------------|------------|
//  | zero_sized  | `unit`, empty structs, empty arrays          | nothing    |
//  | scalar      | numbers, `bool`, `byte`                      | a register |
//  | address     | `&T`, `*T`, `cell[T]`                        | a register |
//  | handle      | `task`, `crew`, `generator`                  | a register |
//  | descriptor  | `str`, `slice[T]`                            | an address |
//  | aggregate   | struct, tuple, array, sum, `fn` value        | an address |
//
//  The internal signature of
//
//      make(a: A, b: B) -> T
//
//  is `make(result_address?, a, b) -> unit` when `T` travels as an address
//  (descriptor or aggregate), and `make(a, b) -> T` otherwise. An address
//  parameter is private storage that the callee owns for the duration of the
//  call and drops itself; it is not a `&T` and carries no loan. A zero-sized
//  value occupies no operand.
//
//  Classification reads `runtime::value_layouts`, so it agrees with the
//  storage form and, later, the backends. It does not look at ownership.
// ==========================================================================

/// How one value crosses a call boundary.
enum class pass_class : uint8_t {
  zero_sized, ///< No bytes; no operand.
  scalar,     ///< One register.
  address,    ///< A reference, raw pointer, or cell: one register.
  handle,     ///< A task, crew, or generator handle: one register.
  descriptor, ///< `{len, data}`: transported by address.
  aggregate,  ///< Inline bytes: transported by address.
};

/// Whether values of this class travel as the address of caller-owned bytes.
[[nodiscard]] constexpr auto is_indirect(pass_class cls) -> bool {
  return cls == pass_class::descriptor || cls == pass_class::aggregate;
}

/// Short stable name, used by dumps and diagnostics.
[[nodiscard]] auto pass_class_name(pass_class cls) -> const char *;

/// One value's size and the way it crosses a call boundary.
struct abi_value {
  semantic::type_id type = semantic::k_unknown_type;
  pass_class cls = pass_class::zero_sized;
  size_t size = 0;
  size_t align = 1;

  [[nodiscard]] auto operator==(const abi_value &) const -> bool = default;
};

/// What an internal operand is.
enum class abi_operand_role : uint8_t {
  result_address, ///< Hidden: where the callee builds an indirect result.
  argument,       ///< A source parameter.
};

/// One operand of the internal signature, in order.
struct abi_operand {
  abi_operand_role role = abi_operand_role::argument;
  /// The source parameter this operand carries; `npos` for the result address.
  size_t source_index = static_cast<size_t>(-1);
  abi_value value;
  /// Whether the operand is an address of private storage rather than a value
  /// in a register.
  bool by_address = false;
};

/// The internal signature of a callable.
struct abi_signature {
  abi_value result;
  /// Every source parameter in source order, zero-sized ones included.
  std::vector<abi_value> params;

  /// The callee builds its result in caller storage reached by a hidden first
  /// operand.
  [[nodiscard]] auto returns_through_address() const -> bool {
    return is_indirect(result.cls);
  }
  /// The operands the callee actually receives: the hidden result address
  /// when there is one, then each non-zero-sized parameter.
  [[nodiscard]] auto operands() const -> std::vector<abi_operand>;

  [[nodiscard]] auto operator==(const abi_signature &) const -> bool = default;

  /// Whether a call made against `other` reaches this callee correctly: the
  /// same operands, each with the same class, size and alignment. Type ids
  /// may differ (a `&mut T` passed for a `&T`).
  [[nodiscard]] auto agrees_with(const abi_signature &other) const -> bool;
};

/// Classifies one concrete type.
[[nodiscard]] auto classify_value(runtime::value_layouts &layouts,
                                  semantic::type_id type)
    -> std::expected<abi_value, runtime::layout_error>;

/// The signature of a callable with these parameter types and result type.
[[nodiscard]] auto classify_signature(runtime::value_layouts &layouts,
                                      std::span<const semantic::type_id> params,
                                      semantic::type_id result)
    -> std::expected<abi_signature, runtime::layout_error>;

/// The signature of a call through a value of `fn` type `fn_type`. Direct and
/// indirect callables share one signature rule, so a function and a `fn`
/// value made from it agree.
[[nodiscard]] auto classify_fn_type(runtime::value_layouts &layouts,
                                    const semantic::type_table &types,
                                    semantic::type_id fn_type)
    -> std::expected<abi_signature, runtime::layout_error>;

/// The signature of a lowered function, including a synthesized one (a task
/// thunk, a generator, an `async def`): those return a handle, which is a
/// direct class.
[[nodiscard]] auto classify_function(runtime::value_layouts &layouts,
                                     const hir_function &function)
    -> std::expected<abi_signature, runtime::layout_error>;

/// A one-line rendering such as `(result@, aggregate, scalar) -> aggregate`.
[[nodiscard]] auto format_signature(const abi_signature &signature) -> std::string;

} // namespace cinder::hir
