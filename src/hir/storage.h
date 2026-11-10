#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/hir/abi.h"
#include "src/hir/ids.h"
#include "src/parser/ast.h"
#include "src/parser/source_location.h"
#include "src/parser/token.h"
#include "src/runtime/value_layout.h"
#include "src/semantic/checker_results.h"
#include "src/semantic/type_table.h"

namespace cinder::hir {

// ==========================================================================
//  Shared storage form (representation v2).
//
//  Typed HIR says what a function computes. The storage form says where every
//  value lives while it is computed: which bytes are built in place, which
//  are copied or moved, and which caller storage receives a call's result.
//  Both backends read this form, so a decision such as "build this struct
//  directly in the caller's result bytes" is made once.
//
//  A function lowers to a flat list of `storage_slot`s and a straight-line
//  list of `storage_op`s. Control flow joins the form in a later step; until
//  then a function whose body needs it is refused with a named reason rather
//  than approximated.
//
//  Initialization and ownership are tracked apart from the bytes. A move
//  copies bytes and invalidates the source in the form's initialization
//  state; it does not create a second owner. The verifier
//  (`storage_verify.h`) replays that state.
// ==========================================================================

/// What lowering and the verifier ask of the checked program: the type table,
/// concrete layouts, and the two ownership facts that decide how a value is
/// transferred. Holds references; both must outlive it.
struct storage_env {
  const semantic::checked_types &checked;
  runtime::value_layouts &layouts;

  /// Whether a value of `type` is copied rather than moved.
  [[nodiscard]] auto is_copy(semantic::type_id type) const -> bool;
  /// Whether dropping a value of `type` does anything. `fn` values count:
  /// a closure may carry drop glue for its environment.
  [[nodiscard]] auto needs_drop(semantic::type_id type) -> bool;
  /// The checker's drop plan for `type`, or null when it has none of its own
  /// (a type with no drop, or a tuple or array that only contains droppables).
  [[nodiscard]] auto drop_plan(semantic::type_id type) const
      -> const semantic::drop_plan *;
  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return checked.types;
  }
};

/// Index of a `storage_slot` in its function.
using storage_id = uint32_t;
inline constexpr storage_id k_invalid_storage = static_cast<storage_id>(-1);

/// Where a slot's bytes come from and how long they last.
enum class storage_class : uint8_t {
  frame_local,         ///< A local or temporary in this function's frame.
  parameter,           ///< An incoming parameter: private to this function.
  caller_result,       ///< The caller's uninitialized result destination.
  outgoing_argument,   ///< Caller-built storage consumed by exactly one call.
  global,              ///< A program-wide `static`.
  owner_field,         ///< A field of a longer-lived owner.
  closure_environment, ///< A captured value in a closure's environment.
  coroutine_frame,     ///< A value that lives across a suspension.
};

[[nodiscard]] auto storage_class_name(storage_class cls) -> const char *;

/// One region of storage holding a value of one type.
struct storage_slot {
  storage_id id = k_invalid_storage;
  storage_class cls = storage_class::frame_local;
  semantic::type_id type = semantic::k_unknown_type;
  /// The source name, for dumps; empty for a temporary.
  std::string name{};
  /// The binding this slot holds, when it holds one.
  symbol_id symbol = k_invalid_symbol_id;
  source_span span{};
  /// Whether the slot holds a value before the first operation: parameters
  /// and globals do, a fresh local or result does not.
  bool initially_initialized = false;
  /// The slot whose lifetime bounds this one. A slot that owns its own bytes
  /// names itself; an `owner_field` names the owner it is part of.
  storage_id owner = k_invalid_storage;
};

/// One checked step from a value to a part of it.
enum class projection_kind : uint8_t {
  member,  ///< A struct field or tuple element, by index.
  element, ///< An array element at a constant index.
  payload, ///< One payload slot of a named sum variant.
  deref,   ///< The referent of an address held in the value.
};

struct projection {
  projection_kind kind = projection_kind::member;
  /// Member index, element index, or payload slot index.
  size_t index = 0;
  /// The field name for a struct member, the variant name for a payload.
  std::string name{};

  [[nodiscard]] auto operator==(const projection &) const -> bool = default;
};

/// A typed part of a slot: `base`, then each projection in turn. `type` is the
/// type of the part the projections arrive at.
struct place {
  storage_id base = k_invalid_storage;
  std::vector<projection> steps{};
  semantic::type_id type = semantic::k_unknown_type;

  [[nodiscard]] auto operator==(const place &) const -> bool = default;
};

/// A scalar value an operation reads.
enum class operand_kind : uint8_t {
  read,       ///< The scalar-class value stored at a place.
  address_of, ///< The address of a place.
  constant,   ///< A literal.
};

struct operand {
  operand_kind kind = operand_kind::read;
  place where{}; ///< `read` and `address_of`.
  /// A `read` that takes ownership: the source is invalid afterwards. Set for
  /// a handle that is not `copy`; scalars and addresses are never consumed.
  bool consume = false;
  semantic::type_id type = semantic::k_unknown_type;
  token_kind literal = token_kind::eof; ///< `constant`.
  std::string text{};                     ///< `constant`: the spelling.
};

/// What a `compute` operation evaluates.
enum class compute_kind : uint8_t {
  constant, ///< A literal.
  copy,     ///< The operand itself.
  binary,
  unary,
  cast,
};

/// The target of a call.
struct call_target {
  /// A statically known function. For an indirect call, `name` is empty and
  /// `callable` is the place holding the `fn` value.
  std::string name{};
  std::optional<std::string> owner_module{};
  std::optional<place> callable{};

  [[nodiscard]] auto is_direct() const -> bool { return !callable.has_value(); }
};

/// One call argument. A scalar, address, or handle argument is an operand; a
/// descriptor or aggregate argument is the outgoing slot that holds it, and
/// the call consumes that slot.
struct call_argument {
  size_t source_index = 0;
  pass_class cls = pass_class::scalar;
  operand value{};       ///< Direct classes.
  place slot{};          ///< Indirect classes: initialized before the call.
};

enum class op_kind : uint8_t {
  compute,         ///< `dest` <- a scalar computed from operands.
  init_empty,      ///< `dest` <- a zero-sized value.
  copy_into,       ///< `dest` <- bytes of `source`; both stay initialized.
  move_into,       ///< `dest` <- bytes of `source`; `source` is invalidated.
  publish_variant, ///< `dest` (a sum) becomes initialized with `variant`.
  drop_in_place,   ///< Runs the drop of the value at `dest`; it is then dead.
  call_into,       ///< Calls `callee`; the result is built in `dest`.
  storage_dead,    ///< Ends the lifetime of slot `slot`.
  return_value,    ///< Leaves the function; see `value`.
};

[[nodiscard]] auto op_kind_name(op_kind kind) -> const char *;

/// One step of the function. Only the fields its `kind` names are used.
struct storage_op {
  op_kind kind = op_kind::compute;
  source_span span{};
  place dest{};
  place source{};
  // compute
  compute_kind computation = compute_kind::copy;
  std::vector<operand> operands{};
  token_kind literal = token_kind::eof;
  std::string text{};
  ast::binary_op binary = ast::binary_op::add;
  ast::unary_op unary = ast::unary_op::neg;
  // publish_variant
  std::string variant{};
  // call_into
  /// The call is a type's own `drop`, synthesized where a value ends. Its one
  /// argument is the address of the value, which stays in place; the verifier
  /// records that the value's own drop has run.
  bool is_drop = false;
  call_target callee{};
  abi_signature signature{};
  std::vector<call_argument> args{};
  // storage_dead
  storage_id slot = k_invalid_storage;
  // return_value: the direct result, absent when the result is built in the
  // `caller_result` slot or is zero-sized.
  std::optional<operand> value{};
};

/// A function in storage form.
struct storage_function {
  std::string name;
  abi_signature signature;
  std::vector<storage_slot> slots;
  /// The slot a descriptor or aggregate result is built in; invalid when the
  /// result is returned directly or is zero-sized.
  storage_id result_slot = k_invalid_storage;
  /// The slot of each source parameter, in order.
  std::vector<storage_id> param_slots;
  std::vector<storage_op> ops;

  [[nodiscard]] auto slot(storage_id id) const -> const storage_slot & {
    return slots[id];
  }
};

/// The functions of one module in storage form.
struct storage_module {
  std::string name;
  std::vector<storage_function> functions;

  [[nodiscard]] auto find(std::string_view function_name) const
      -> const storage_function *;
};

/// A stable, line-per-operation text rendering. Slots are named `s<id>` and
/// listed first; the rendering depends only on the form, so tests can compare
/// it exactly.
[[nodiscard]] auto dump(const storage_function &function,
                        const semantic::type_table &types) -> std::string;

[[nodiscard]] auto dump(const storage_module &module,
                        const semantic::type_table &types) -> std::string;

} // namespace cinder::hir
