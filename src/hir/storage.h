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
//  A function lowers to a flat list of `storage_slot`s and a list of
//  `storage_op`s. `if`, `while`, `break`, `continue`, and early `return` are
//  structured markers in that list; a construct the form does not express yet
//  (`match`, `while let`) is refused with a named reason rather than
//  approximated.
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
  semantic::checked_types &checked;
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
  /// What dereferencing a value of `type` reaches: the referent of a
  /// reference or raw pointer, or the `T` a `cell[T]`/`cell_mut[T]` is the
  /// address of. None for any other type.
  [[nodiscard]] auto pointee(semantic::type_id type) const
      -> std::optional<semantic::type_id>;
  /// Whether a `from` value can be read as a `to`: `from` is a
  /// `cell[T]`/`cell_mut[T]` and `to` a reference to the same `T`, `&mut T`
  /// only from a `cell_mut[T]`. Both are the address of the `T`.
  [[nodiscard]] auto cell_reads_as_reference(semantic::type_id from,
                                             semantic::type_id to) const -> bool;
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
  coroutine_frame,     ///< A value that lives across a suspension: the bytes
                       ///< are in the state block the step function received,
                       ///< `env_offset` bytes in.
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
  /// `closure_environment` and `coroutine_frame`: the slot's bytes are in the
  /// environment (state) block the function received, `env_offset` bytes in. When `env_indirect` is set
  /// the environment holds the address of the variable instead, and the slot is
  /// the variable it points at.
  size_t env_offset = 0;
  bool env_indirect = false;
  /// The value here owes no drop: the iterator a `for` loop makes from a
  /// borrowed collection, which holds a bitwise copy of what it borrows.
  bool owns_nothing = false;
};

/// One checked step from a value to a part of it.
enum class projection_kind : uint8_t {
  member,  ///< A struct field or tuple element, by index.
  element, ///< An array element at a constant index.
  payload, ///< One payload slot of a named sum variant.
  deref,   ///< The referent of an address held in the value.
  length,  ///< The length word of a `str`/`slice` descriptor (`usize`).
  data,    ///< The data address of a `str`/`slice` descriptor (`*T`).
};

struct projection {
  projection_kind kind = projection_kind::member;
  /// Member index, element index, or payload slot index.
  size_t index = 0;
  /// The field name for a struct member, the variant name for a payload.
  std::string name{};
  /// `length` and `data` only: the type the part is read as, since the
  /// descriptor's layout has no members to name it.
  semantic::type_id type = semantic::k_unknown_type;

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
  /// `operands[0]` (a raw pointer) advanced by `operands[1]` elements of
  /// `stride` bytes each: the address of `p[i]`.
  pointer_offset,
  /// `operands[0]` when it is below `operands[1]`, and otherwise a panic
  /// with `index_out_of_bounds`: an index checked against a length. All
  /// three are `usize`.
  checked_index,
  /// `operands[0]` when it is at most `operands[1]`, and otherwise the same
  /// panic: a range bound checked against the length or the other bound.
  checked_end,
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

/// How a closure's environment holds one captured variable.
enum class capture_kind : uint8_t {
  move,      ///< The value moves in; the closure owns it and drops it.
  copy,      ///< A bitwise copy. The source stays valid and owned by its scope.
  reference, ///< The environment holds the address of the variable.
};

[[nodiscard]] auto capture_kind_name(capture_kind kind) -> const char *;

/// One captured variable, as `make_closure` stores it.
struct closure_capture {
  place source{};      ///< The variable in the enclosing function.
  capture_kind kind = capture_kind::copy;
  size_t offset = 0;   ///< Bytes into the environment block.
};

enum class op_kind : uint8_t {
  compute,         ///< `dest` <- a scalar computed from operands.
  init_empty,      ///< `dest` <- a zero-sized value.
  string_literal,  ///< `dest` (a `str`) <- a descriptor of the bytes `text`,
                   ///< which live in static data for the program's lifetime.
  copy_into,       ///< `dest` <- bytes of `source`; both stay initialized.
  move_into,       ///< `dest` <- bytes of `source`; `source` is invalidated.
  replicate,       ///< `dest` (an array) <- every element a copy of `source`,
                   ///< which is its element 0 and already written. The
                   ///< element is copied, so it needs no drop.
  publish_variant, ///< `dest` (a sum) becomes initialized with `variant`.
  test_variant,    ///< `dest` (a `bool`) <- whether the sum at `source` holds
                   ///< `variant`. `source` stays initialized.
  drop_in_place,   ///< Runs the drop of the value at `dest`; it is then dead.
  zero_fill,       ///< `dest` (an `uninit[T, N]` buffer) <- zero bytes. The
                   ///< buffer promises no value, so this counts as writing it.
  forget,          ///< The value at `dest` ends without a drop: what it owned
                   ///< is dropped through a raw pointer instead (an array's
                   ///< elements). No code; the verifier treats it as a drop.
  call_into,       ///< Calls `callee`; the result is built in `dest`.
  make_closure,    ///< `dest` (a `fn` value) <- `{entry, env, glue}`; see below.
  release_environment, ///< Frees the environment block this closure body or
                       ///< drop glue received. Only the glue does this.
  // Generators. A `generator def` lowers to a constructor and a step function.
  // The step's frame is its state block: every local is a `coroutine_frame`
  // slot at a fixed offset in it, so nothing needs saving at a suspension.
  make_generator,   ///< `dest` (a generator) <- a new coroutine whose step is
                    ///< `entry`; `env_size`/`env_align` size its state block
                    ///< and `captures` fill it, as for `make_closure`.
  generator_resume, ///< `dest` (a `uint64`) <- the status after resuming the
                    ///< generator at `source` once.
  generator_take,   ///< `dest` <- the item the generator at `source` last
                    ///< yielded, moved out of the producer's state block. The
                    ///< coroutine then holds no item.
  coroutine_has_item,  ///< `dest` (a `bool`) <- whether the coroutine at
                       ///< `source` holds an item that was handed over and not
                       ///< yet taken.
  coroutine_dispatch,  ///< Step only, first: jumps to the resume point of
                       ///< `source` (a `uint64`); `depth` points exist.
  coroutine_suspend,   ///< Step only: hands the item at `source` (consumed) to
                       ///< the resumer of `dest` (the coroutine) and returns;
                       ///< resumption continues here. `depth` numbers the point
                       ///< from 1.
  coroutine_await,     ///< Step only, in a task: waits for the task at `source`,
                       ///< or, when `waits` is `all` or `any`, for a tuple of
                       ///< tasks to all finish or for one of them to.
                       ///< `dest` is the coroutine. If the wait is not over
                       ///< the step returns `pending` and, once woken,
                       ///< resumes here and polls again. `depth` numbers the
                       ///< point from 1, as for a suspension.
  task_block_on_group, ///< Not in a step: blocks the thread until all (or, for
                       ///< `waits == any`, one) of the tuple of tasks at
                       ///< `source` have finished.
  coroutine_cancelled, ///< Step only: `dest` (a `bool`) <- whether the
                       ///< coroutine at `source` was resumed to unwind.
  coroutine_finish,    ///< Step only: marks the coroutine at `source` done.
  storage_dead,    ///< Ends the lifetime of slot `slot`.
  return_value,    ///< Leaves the function; see `value`.
  unreachable,     ///< Never runs: the operation before it (a call that
                   ///< returns `never`) does not return. Ends the path.
  // Structured control flow. The markers nest like brackets in the flat
  // operation list; the verifier joins the initialization state where
  // branches meet.
  if_begin,   ///< Runs the operations up to `else_begin` or `if_end` when
              ///< `condition` is true.
  else_begin, ///< Starts the operations that run when it is false.
  if_end,     ///< Ends the conditional; the two paths join here.
  loop_begin, ///< Starts operations that repeat until an `exit` leaves them.
  loop_end,   ///< Ends the loop body and returns to `loop_begin`.
  block_begin, ///< Starts a region that an `exit` can leave early.
  block_end,   ///< Ends the region; the paths that left it join here.
  exit,        ///< Leaves the `depth`-th enclosing loop or block (0 is the
               ///< innermost); `if` regions are not counted.
};

[[nodiscard]] auto op_kind_name(op_kind kind) -> const char *;

/// What an await waits for.
enum class wait_mode : uint8_t {
  one, ///< A single task.
  all, ///< Every task of a tuple (`await par`).
  any, ///< The first task of a tuple to finish (`await race`).
  crew, ///< Every task spawned into one `crew` (the end of a crew block).
};

[[nodiscard]] auto wait_mode_name(wait_mode mode) -> const char *;

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
  /// `pointer_offset`: the byte size of one element.
  size_t stride = 0;
  // publish_variant, test_variant
  std::string variant{};
  // call_into
  /// The call is a type's own `drop`, synthesized where a value ends. Its one
  /// argument is the address of the value, which stays in place; the verifier
  /// records that the value's own drop has run.
  bool is_drop = false;
  call_target callee{};
  /// `copy_into` only: a bitwise copy of a value that is not `copy`, out of a
  /// reference, into the receiver of a call whose result owns nothing
  /// (`storage_slot::owns_nothing`).
  bool shallow = false;
  abi_signature signature{};
  std::vector<call_argument> args{};
  // if_begin
  operand condition{};
  // exit
  size_t depth = 0;
  // coroutine_await, task_block_on_group
  wait_mode waits = wait_mode::one;
  // make_closure, make_generator: `entry` and `glue` name functions of the module that take the
  // environment as their first operand (`abi_signature::takes_environment`).
  // The environment is one block of `env_size` bytes, allocated here when it is
  // not empty and released by `glue`; `captures` fill it. `glue` is empty only
  // when the environment is empty.
  std::string entry{};
  std::string glue{};
  size_t env_size = 0;
  size_t env_align = 1;
  std::vector<closure_capture> captures{};
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
  /// The function is a generator's step: it takes the state block as its
  /// environment, then the resume index and the coroutine, and returns the
  /// status word. Its locals are `coroutine_frame` slots.
  bool coroutine_step = false;
  /// The function builds its module's statics: their `global` slots start
  /// unwritten and it writes each one once. Every other function reads them.
  bool static_init = false;
  std::vector<storage_op> ops;

  [[nodiscard]] auto slot(storage_id id) const -> const storage_slot & {
    return slots[id];
  }
};

/// A `static` the module declares, held in program-wide storage. `name` is
/// unique across the program, and a `global` slot names it the same way.
struct storage_global {
  std::string name;
  semantic::type_id type = semantic::k_unknown_type;
  source_span span{};
};

/// The name of the function that builds a module's statics. It contains a
/// space, so no source function can have it.
inline constexpr std::string_view k_static_init_name = "static init";

/// The functions of one module in storage form.
struct storage_module {
  std::string name;
  std::vector<storage_function> functions;
  /// The statics the module declares, in declaration order. When there are
  /// any, the function `k_static_init_name` builds them; a program runs each
  /// module's once, in module order, before anything else.
  std::vector<storage_global> globals;

  [[nodiscard]] auto find(std::string_view function_name) const
      -> const storage_function *;
};

/// The program-wide name of function `name` of module `module` in a program
/// whose entry module is `entry`: the name itself for the entry module's
/// functions, `module::name` for any other module's. Both backends key their
/// function tables by it, so two modules may use one function name, and a
/// host finds the entry module's functions under their own names.
[[nodiscard]] auto program_function_name(std::string_view entry,
                                         std::string_view module,
                                         std::string_view name) -> std::string;

/// The program-wide name of the function a direct call names, from the
/// module the call is in. A callee with no owner module is in that module.
[[nodiscard]] auto program_callee_name(std::string_view entry,
                                       std::string_view module,
                                       const call_target &callee) -> std::string;

/// A stable, line-per-operation text rendering. Slots are named `s<id>` and
/// listed first; the rendering depends only on the form, so tests can compare
/// it exactly.
[[nodiscard]] auto dump(const storage_function &function,
                        const semantic::type_table &types) -> std::string;

[[nodiscard]] auto dump(const storage_module &module,
                        const semantic::type_table &types) -> std::string;

} // namespace cinder::hir
