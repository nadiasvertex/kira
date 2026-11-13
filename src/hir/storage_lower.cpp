#include "src/hir/storage_lower.h"

#include "src/hir/captures.h"
#include "src/hir/traversal.h"
#include "src/parser/text_escape.h"
#include "src/runtime/coroutine.h"
#include "src/runtime/scheduler.h"

#include <algorithm>
#include <deque>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cinder::hir {

namespace {

using semantic::type_id;

template <typename T> [[nodiscard]] auto as(const hir_node &node) -> const T & {
  return static_cast<const T &>(node);
}

[[nodiscard]] auto describe(hir_node_kind kind) -> std::string {
  switch (kind) {
  case hir_node_kind::hir_match:
    return "a `match` expression";
  case hir_node_kind::hir_lambda:
    return "a closure";
  case hir_node_kind::hir_block:
    return "a block expression";
  case hir_node_kind::hir_index:
    return "an index expression";
  case hir_node_kind::hir_while_let:
    return "a `while let` loop";
  case hir_node_kind::hir_let_else:
    return "`let ... else`";
  case hir_node_kind::hir_yield:
    return "`yield` or `await`";
  case hir_node_kind::hir_contract_check:
    return "a contract check";
  case hir_node_kind::hir_host_call:
    return "a host call";
  case hir_node_kind::hir_generator_next:
    return "a generator step";
  default:
    return "this construct (HIR node kind " +
           std::to_string(static_cast<int>(kind)) + ")";
  }
}

using lower_failure = std::unexpected<storage_lower_error>;

[[nodiscard]] auto refuse(const source_span &span, std::string message)
    -> lower_failure {
  return std::unexpected(storage_lower_error{
      .kind = storage_lower_error_kind::unsupported_construct,
      .span = span,
      .message = std::move(message)});
}

[[nodiscard]] auto malformed(const source_span &span, std::string message)
    -> lower_failure {
  return std::unexpected(
      storage_lower_error{.kind = storage_lower_error_kind::malformed,
                          .span = span,
                          .message = std::move(message)});
}

/// Whether evaluating `expr` can have an effect or observe one.
[[nodiscard]] auto is_pure(const hir_expr &expr) -> bool {
  switch (expr.kind) {
  case hir_node_kind::hir_literal:
  case hir_node_kind::hir_local_ref:
  case hir_node_kind::hir_global_ref:
    return true;
  case hir_node_kind::hir_field:
    return is_pure(*as<hir_field>(expr).object);
  case hir_node_kind::hir_tuple_index:
    return is_pure(*as<hir_tuple_index>(expr).object);
  case hir_node_kind::hir_variant_payload:
    return is_pure(*as<hir_variant_payload>(expr).object);
  case hir_node_kind::hir_unary:
    return is_pure(*as<hir_unary>(expr).operand);
  case hir_node_kind::hir_cast:
    return is_pure(*as<hir_cast>(expr).operand);
  case hir_node_kind::hir_binary:
    return is_pure(*as<hir_binary>(expr).lhs) &&
           is_pure(*as<hir_binary>(expr).rhs);
  default:
    return false;
  }
}

/// One captured variable as the closure's environment holds it.
struct environment_entry {
  symbol_id symbol = k_invalid_symbol_id;
  type_id type = semantic::k_unknown_type; ///< The variable's own type.
  std::string name;
  capture_kind kind = capture_kind::copy;
  size_t offset = 0;
};

/// The typed layout of one closure's environment block.
struct environment_plan {
  std::vector<environment_entry> entries;
  size_t size = 0;
  size_t align = 1;
};

/// What a lowerer needs of a function or closure body.
struct body_shape {
  std::string name;
  const std::vector<hir_param> *params = nullptr;
  type_id return_type = semantic::k_unknown_type;
  const hir_block *body = nullptr;
  source_span span{};
  bool coroutine = false;
  /// A generator's step function: `generator` is the generator type it steps.
  /// Its locals live in the state block its caller supplies.
  bool step = false;
  type_id generator = semantic::k_unknown_type;
  /// A closure body: the environment it reads its captures from.
  const environment_plan *environment = nullptr;
  /// A closure's drop glue: releases the environment block when it ends.
  bool releases_environment = false;
  /// The module's statics, which this function builds in their own storage.
  const std::vector<hir_static_global> *statics = nullptr;
  /// The first parameter is `self: &T`, but the function owns the `T`
  /// (`hir_function::owns_receiver`): it is a `T` passed by address.
  bool owns_receiver = false;
};

/// The closure bodies and drop glue that lowering one function produced.
struct closure_sink {
  std::vector<storage_function> functions;
  /// A named function used as a value is a closure over a function that
  /// forwards to it, made once per function: its name, by the function's key.
  std::unordered_map<std::string, std::string> forwarders;
  /// The parameters and bodies of those forwarding functions, which their
  /// lowering points into.
  std::deque<std::vector<hir_param>> forwarder_params;
  std::vector<ptr<hir_block>> forwarder_bodies;
};

[[nodiscard]] auto shape_of(const hir_function &function) -> body_shape {
  return body_shape{.name = function.name,
                    .params = &function.params,
                    .return_type = function.return_type,
                    .body = function.body.get(),
                    .span = function.span,
                    .coroutine = function.is_coroutine(),
                    .owns_receiver = function.owns_receiver};
}

class function_lowerer {
public:
  function_lowerer(body_shape shape, storage_env &env, closure_sink &closures)
      : fn_(std::move(shape)), env_(env), closures_(closures) {}

  /// The size and alignment of the state block a step function lowered to.
  [[nodiscard]] auto state_size() const -> size_t { return state_size_; }
  [[nodiscard]] auto state_align() const -> size_t { return state_align_; }

  [[nodiscard]] auto run() -> std::expected<storage_function, storage_lower_error> {
    if (fn_.step) {
      return run_step();
    }
    if (fn_.coroutine) {
      return refuse(fn_.span,
                    "a task body needs suspension-aware storage");
    }
    out_.name = fn_.name;
    auto param_types = std::vector<type_id>{};
    for (const auto &param : *fn_.params) {
      param_types.push_back(param.type);
    }
    if (fn_.owns_receiver && !param_types.empty()) {
      const auto owned = env_.pointee(param_types.front());
      if (!owned.has_value()) {
        return refuse(fn_.span, "a receiver it owns that is not a reference");
      }
      param_types.front() = *owned;
    }
    auto signature =
        classify_signature(env_.layouts, param_types, fn_.return_type);
    if (!signature.has_value()) {
      return layout_failure(fn_.span, signature.error());
    }
    signature->takes_environment = fn_.environment != nullptr;
    out_.signature = std::move(*signature);
    if (out_.signature.returns_through_address()) {
      out_.result_slot = new_slot(storage_class::caller_result,
                                  fn_.return_type, "", k_invalid_symbol_id,
                                  fn_.span, false);
    }
    for (size_t i = 0; i < fn_.params->size(); ++i) {
      const auto &param = (*fn_.params)[i];
      const auto id = new_slot(storage_class::parameter, param_types[i],
                               param.name, param.symbol, fn_.span, true);
      out_.param_slots.push_back(id);
      if (i == 0 && fn_.owns_receiver) {
        owned_receiver_ = receiver{.symbol = param.symbol, .slot = id};
        continue;
      }
      locals_[param.symbol] = id;
    }
    if (fn_.environment != nullptr) {
      for (const auto &entry : fn_.environment->entries) {
        const auto id = new_slot(storage_class::closure_environment, entry.type,
                                 entry.name, entry.symbol, fn_.span, true);
        out_.slots[id].env_offset = entry.offset;
        out_.slots[id].env_indirect = entry.kind == capture_kind::reference;
        locals_[entry.symbol] = id;
      }
    }
    if (fn_.statics != nullptr) {
      out_.static_init = true;
      if (auto ok = lower_statics(*fn_.statics); !ok.has_value()) {
        return std::unexpected(ok.error());
      }
    }
    if (fn_.body != nullptr) {
      const auto &stmts = fn_.body->stmts;
      for (size_t i = 0; i < stmts.size(); ++i) {
        const auto &stmt = *stmts[i];
        // The body's last expression statement is the value it returns (a
        // trailing `match` or `if`, which HIR types as the result), unless
        // the result is zero-sized or the expression does not return.
        const auto *tail =
            i + 1 == stmts.size() && stmt.kind == hir_node_kind::hir_expr_stmt &&
                    out_.signature.result.cls != pass_class::zero_sized &&
                    !always_leaves(stmt)
                ? as<hir_expr_stmt>(stmt).expr.get()
                : nullptr;
        if (auto ok = tail != nullptr ? lower_return_value(*tail, stmt.span)
                                      : lower_stmt(stmt);
            !ok.has_value()) {
          return std::unexpected(ok.error());
        }
        if (terminated_) {
          break;
        }
      }
    }
    if (!terminated_) {
      if (out_.signature.result.cls != pass_class::zero_sized) {
        return malformed(fn_.span, "the body can end without returning a value");
      }
      if (auto ended = flush_pending(fn_.span); !ended.has_value()) {
        return std::unexpected(ended.error());
      }
      if (fn_.releases_environment) {
        emit(storage_op{.kind = op_kind::release_environment,
                        .env_size = fn_.environment->size},
             fn_.span);
      }
      emit_return(std::nullopt, fn_.span);
    }
    return std::move(out_);
  }

private:
  using result = std::expected<void, storage_lower_error>;

  /// Builds each static in its `global` slot, which starts unwritten here.
  /// A static's value is a compile-time constant, so it reads no other.
  [[nodiscard]] auto lower_statics(const std::vector<hir_static_global> &statics)
      -> result {
    for (const auto &global : statics) {
      const auto id = new_slot(storage_class::global, global.type, global.name,
                               k_invalid_symbol_id, global.value->span, false);
      globals_.emplace(global.name, id);
      if (auto ok = lower_into(*global.value, whole(id)); !ok.has_value()) {
        return ok;
      }
      if (auto ended = flush_pending(global.value->span); !ended.has_value()) {
        return ended;
      }
    }
    return {};
  }

  enum class loop_frame_kind : uint8_t { loop, body };

  struct pending_temp {
    storage_id slot = k_invalid_storage;
    bool consumed = false;
  };

  // ------------------------------------------------------------------
  //  Slots, places, emission
  // ------------------------------------------------------------------

  [[nodiscard]] auto layout_failure(const source_span &span,
                                    const runtime::layout_error &error) const
      -> lower_failure {
    return std::unexpected(storage_lower_error{
        .kind = storage_lower_error_kind::layout_failed,
        .span = span,
        .message = error.message});
  }

  auto new_slot(storage_class cls, type_id type, std::string name,
                symbol_id symbol, const source_span &span, bool initialized)
      -> storage_id {
    if (fn_.step && cls == storage_class::frame_local) {
      // A generator's frame is its state block, which outlives a suspension.
      cls = storage_class::coroutine_frame;
    }
    const auto id = static_cast<storage_id>(out_.slots.size());
    out_.slots.push_back(storage_slot{.id = id,
                                      .cls = cls,
                                      .type = type,
                                      .name = std::move(name),
                                      .symbol = symbol,
                                      .span = span,
                                      .initially_initialized = initialized,
                                      .owner = id});
    return id;
  }

  [[nodiscard]] auto whole(storage_id id) const -> place {
    return place{.base = id, .steps = {}, .type = out_.slots[id].type};
  }

  /// A temporary that lives to the end of the statement.
  auto new_temp(type_id type, const source_span &span) -> storage_id {
    const auto id = new_slot(storage_class::frame_local, type, "",
                             k_invalid_symbol_id, span, false);
    pending_.push_back(pending_temp{.slot = id});
    return id;
  }

  auto consumed(storage_id id) -> void {
    for (auto &temp : pending_) {
      if (temp.slot == id) {
        temp.consumed = true;
      }
    }
  }

  auto emit(storage_op op, const source_span &span) -> void {
    op.span = span;
    out_.ops.push_back(std::move(op));
  }

  /// Ends the statement's temporaries, newest first. A temporary that still
  /// holds a value needing a drop is refused: HIR schedules the drops of the
  /// values it knows about as explicit calls, and translating those to places
  /// belongs to ownership integration, so lowering does not invent a second
  /// drop for a value it cannot tie to the first.
  auto flush_pending(const source_span &span) -> result {
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
      const auto &slot = out_.slots[it->slot];
      if (!it->consumed && env_.needs_drop(slot.type)) {
        pending_.clear();
        return refuse(span, "a temporary of type `" +
                                types().display(slot.type) +
                                "` that needs a drop");
      }
      auto dead = storage_op{.kind = op_kind::storage_dead};
      dead.slot = it->slot;
      emit(std::move(dead), span);
    }
    pending_.clear();
    return {};
  }

  auto emit_return(std::optional<operand> value, const source_span &span)
      -> void {
    if (fn_.step) {
      // The body is over: the coroutine is marked done and the step reports it.
      emit(storage_op{.kind = op_kind::coroutine_finish,
                      .source = whole(object_slot_)},
           span);
      value = word_constant(runtime::coroutine::raw(runtime::coroutine::status::done));
    }
    auto ret = storage_op{.kind = op_kind::return_value};
    ret.value = std::move(value);
    emit(std::move(ret), span);
    terminated_ = true;
  }

  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return env_.types();
  }

  /// A constant of the word type that generators use for statuses and indexes.
  [[nodiscard]] auto word_constant(uint64_t value) const -> operand {
    return operand{.kind = operand_kind::constant,
                   .type = types().usize_type(),
                   .literal = token_kind::int_lit,
                   .text = std::to_string(value)};
  }

  [[nodiscard]] auto class_of(type_id type, const source_span &span)
      -> std::expected<pass_class, storage_lower_error> {
    const auto value = classify_value(env_.layouts, type);
    if (!value.has_value()) {
      return layout_failure(span, value.error());
    }
    return value->cls;
  }

  [[nodiscard]] auto layout_of(type_id type, const source_span &span)
      -> std::expected<const runtime::value_layout *, storage_lower_error> {
    const auto layout = env_.layouts.layout(type);
    if (!layout.has_value()) {
      return layout_failure(span, layout.error());
    }
    return *layout;
  }

  [[nodiscard]] auto project(place base, projection step, type_id type) const
      -> place {
    base.steps.push_back(std::move(step));
    base.type = type;
    return base;
  }

  [[nodiscard]] auto member_step(size_t index, const std::string &name) const
      -> projection {
    return projection{
        .kind = projection_kind::member, .index = index, .name = name};
  }

  /// Follows references until `where` names a value (`self.x` when `self` is
  /// `&T`).
  [[nodiscard]] auto auto_deref(place where) const -> place {
    for (;;) {
      const auto &entry = types().entry(types().strip_refinement(where.type));
      if (entry.kind != semantic::type_kind::ref_kind) {
        return where;
      }
      where = project(std::move(where),
                      projection{.kind = projection_kind::deref}, entry.result);
    }
  }

  // ------------------------------------------------------------------
  //  Places read from expressions
  // ------------------------------------------------------------------

  /// The place `expr` names, or nullopt when it is a computed value.
  [[nodiscard]] auto lower_place(const hir_expr &expr)
      -> std::expected<std::optional<place>, storage_lower_error> {
    switch (expr.kind) {
    case hir_node_kind::hir_local_ref: {
      if (const auto alias = subjects_.find(as<hir_local_ref>(expr).symbol);
          alias != subjects_.end()) {
        return std::optional<place>{alias->second};
      }
      const auto found = locals_.find(as<hir_local_ref>(expr).symbol);
      if (found == locals_.end()) {
        return std::optional<place>{};
      }
      return retyped(whole(found->second), expr);
    }
    case hir_node_kind::hir_global_ref: {
      const auto &name = as<hir_global_ref>(expr).name;
      auto found = globals_.find(name);
      if (found == globals_.end()) {
        const auto id = new_slot(storage_class::global, expr.type, name,
                                 k_invalid_symbol_id, expr.span, true);
        found = globals_.emplace(name, id).first;
      }
      return retyped(whole(found->second), expr);
    }
    case hir_node_kind::hir_field: {
      const auto &field = as<hir_field>(expr);
      // A computed value is built in a temporary first, which lasts to the
      // end of the statement; one that needs a drop is refused there.
      auto base = container_place(*field.object);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      auto object = std::move(*base);
      const auto layout = layout_of(object.type, expr.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &members = (*layout)->members;
      for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].name == field.field_name) {
          return std::optional<place>{project(std::move(object),
                                              member_step(i, field.field_name),
                                              members[i].type)};
        }
      }
      return malformed(expr.span, "`" + types().display(object.type) +
                                      "` has no field `" + field.field_name +
                                      "`");
    }
    case hir_node_kind::hir_tuple_index: {
      const auto &index = as<hir_tuple_index>(expr);
      // A computed value is built in a temporary first, which lasts to the
      // end of the statement; one that needs a drop is refused there.
      auto base = container_place(*index.object);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      auto object = std::move(*base);
      const auto layout = layout_of(object.type, expr.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &found = **layout;
      if (found.kind == runtime::value_layout_kind::tuple &&
          index.index < found.members.size()) {
        return std::optional<place>{project(std::move(object),
                                            member_step(index.index, ""),
                                            found.members[index.index].type)};
      }
      if (found.kind == runtime::value_layout_kind::array &&
          index.index < found.count) {
        return std::optional<place>{project(
            std::move(object),
            projection{.kind = projection_kind::element, .index = index.index},
            found.element)};
      }
      return refuse(expr.span, "a positional element of `" +
                                   types().display(object.type) + "`");
    }
    case hir_node_kind::hir_variant_payload: {
      const auto &payload = as<hir_variant_payload>(expr);
      auto base = lower_place(*payload.object);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      if (!base->has_value()) {
        return refuse(expr.span, "the payload of a computed value");
      }
      auto object = auto_deref(std::move(**base));
      const auto layout = layout_of(object.type, expr.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto *variant = (*layout)->find_variant(payload.variant_name);
      if (variant == nullptr || payload.index >= variant->payload.size()) {
        return malformed(expr.span, "`" + types().display(object.type) +
                                        "` has no payload slot " +
                                        std::to_string(payload.index) +
                                        " in variant `" +
                                        payload.variant_name + "`");
      }
      return std::optional<place>{project(
          std::move(object),
          projection{.kind = projection_kind::payload,
                     .index = payload.index,
                     .name = payload.variant_name},
          variant->payload[payload.index].type)};
    }
    case hir_node_kind::hir_index:
      return lower_index(as<hir_index>(expr));
    case hir_node_kind::hir_unary: {
      const auto &unary = as<hir_unary>(expr);
      if (unary.op != ast::unary_op::deref) {
        return std::optional<place>{};
      }
      if (const auto owned = owned_receiver_slot(*unary.operand)) {
        return std::optional<place>{whole(*owned)};
      }
      auto base = lower_place(*unary.operand);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      auto pointer = place{};
      if (base->has_value()) {
        pointer = std::move(**base);
      } else {
        const auto temp = new_temp(unary.operand->type, unary.operand->span);
        pointer = whole(temp);
        if (auto ok = lower_into(*unary.operand, pointer); !ok.has_value()) {
          return std::unexpected(ok.error());
        }
      }
      const auto referent = env_.pointee(pointer.type);
      if (!referent.has_value()) {
        return malformed(expr.span, "cannot dereference `" +
                                        types().display(pointer.type) + "`");
      }
      return std::optional<place>{project(
          std::move(pointer), projection{.kind = projection_kind::deref},
          *referent)};
    }
    default:
      return std::optional<place>{};
    }
  }

  /// `x[i]`: a slot of a raw pointer, an element of an array at a constant
  /// index, or an element of an array, `str`, or `slice` at an index checked
  /// at run time. A range index is a value, not a place.
  [[nodiscard]] auto lower_index(const hir_index &index)
      -> std::expected<std::optional<place>, storage_lower_error> {
    if (indexes_raw_pointer(index)) {
      return lower_raw_index(index);
    }
    if (range_of(index) != nullptr) {
      return std::optional<place>{};
    }
    const auto object_layout =
        layout_of(referent_type(index.object->type), index.span);
    if (!object_layout.has_value()) {
      return std::unexpected(object_layout.error());
    }
    auto at = size_t{0};
    const auto constant_index = [&] {
      if (index.index->kind != hir_node_kind::hir_literal) {
        return false;
      }
      const auto &literal = as<hir_literal>(*index.index);
      const auto *end = literal.value.data() + literal.value.size();
      return literal.lit_kind == token_kind::int_lit &&
             std::from_chars(literal.value.data(), end, at).ptr == end;
    }();
    // An `uninit` buffer's slots are raw memory (ch. 38) at every index, so
    // even a constant index reaches its slot through the buffer's address.
    if (!constant_index ||
        (*object_layout)->kind != runtime::value_layout_kind::array ||
        is_uninit(referent_type(index.object->type))) {
      return lower_checked_index(index);
    }
    auto base = container_place(*index.object);
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    auto object = std::move(*base);
    const auto layout = layout_of(object.type, index.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind != runtime::value_layout_kind::array) {
      return std::optional<place>{};
    }
    if (at >= found.count) {
      return malformed(index.span, "element " + std::to_string(at) +
                                       " is past the end of `" +
                                       types().display(object.type) + "`");
    }
    return std::optional<place>{project(
        std::move(object),
        projection{.kind = projection_kind::element, .index = at},
        found.element)};
  }

  /// Whether `type` is an `uninit[T, N]` buffer.
  [[nodiscard]] auto is_uninit(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           entry.name == "uninit";
  }

  /// `type` behind any references.
  [[nodiscard]] auto referent_type(type_id type) const -> type_id {
    for (;;) {
      const auto &entry = types().entry(types().strip_refinement(type));
      if (entry.kind != semantic::type_kind::ref_kind) {
        return type;
      }
      type = entry.result;
    }
  }

  /// The `a..b` or `a..=b` that `index` indexes by, or null.
  [[nodiscard]] static auto range_of(const hir_index &index)
      -> const hir_binary * {
    if (index.index == nullptr || index.index->kind != hir_node_kind::hir_binary) {
      return nullptr;
    }
    const auto &range = as<hir_binary>(*index.index);
    return range.op == ast::binary_op::range ||
                   range.op == ast::binary_op::range_inclusive
               ? &range
               : nullptr;
  }

  /// What indexing a container reads: its length, a pointer to its first
  /// element, and the element's type and stride.
  struct container_view {
    operand length;
    operand data;
    type_id element = semantic::k_unknown_type;
    type_id pointer = semantic::k_unknown_type;
    size_t stride = 0;
  };

  /// The view of the array, `str`, or `slice` at `object`, or nullopt for any
  /// other type. The data pointer is `*mut T` for a `slice_mut` or an array,
  /// `*T` otherwise.
  [[nodiscard]] auto view_of(const place &object, const source_span &span)
      -> std::expected<std::optional<container_view>, storage_lower_error> {
    const auto layout = layout_of(object.type, span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    const auto &entry = types().entry(types().strip_refinement(object.type));
    auto &table = env_.checked.types;
    auto view = container_view{};
    if (found.kind == runtime::value_layout_kind::descriptor) {
      view.element = entry.name == "str" ? table.builtin("byte") : entry.args.at(0);
      view.pointer = table.ptr_to(view.element, entry.name == "slice_mut");
      view.length = read_of(
          descriptor_part(object, projection_kind::length, types().usize_type()),
          false);
      view.data = read_of(descriptor_part(object, projection_kind::data, view.pointer),
                          false);
    } else if (found.kind == runtime::value_layout_kind::array) {
      // An `uninit` buffer's slots are reached the same way, as raw memory.
      view.element = found.element;
      view.pointer = table.ptr_to(view.element, true);
      view.length = word_constant(found.count);
      view.data = operand{.kind = operand_kind::address_of,
                          .where = object,
                          .type = view.pointer};
    } else {
      return std::optional<container_view>{};
    }
    const auto size = env_.layouts.size_of(view.element);
    if (!size.has_value()) {
      return layout_failure(span, size.error());
    }
    view.stride = *size;
    return std::optional<container_view>{std::move(view)};
  }

  /// A `usize` temporary holding `value` checked against `limit`.
  [[nodiscard]] auto checked_word(compute_kind check, operand value,
                                  operand limit, const source_span &span)
      -> storage_id {
    const auto temp = new_temp(types().usize_type(), span);
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = whole(temp);
    op.computation = check;
    op.operands.push_back(std::move(value));
    op.operands.push_back(std::move(limit));
    emit(std::move(op), span);
    return temp;
  }

  /// `xs[i]` at an index known only at run time: the index is checked against
  /// the length, and the element is the slot that many strides past the data
  /// pointer. An element that needs a drop is refused, since a write through
  /// this place would not drop the value it replaces.
  [[nodiscard]] auto lower_checked_index(const hir_index &index)
      -> std::expected<std::optional<place>, storage_lower_error> {
    auto object = container_place(*index.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto view = view_of(*object, index.span);
    if (!view.has_value()) {
      return std::unexpected(view.error());
    }
    if (!view->has_value()) {
      return std::optional<place>{};
    }
    auto &found = **view;
    if (env_.needs_drop(found.element)) {
      return refuse(index.span, "an element of `" + types().display(object->type) +
                                    "` at a run-time index, whose elements "
                                    "need a drop");
    }
    auto at = lower_operand(*index.index, false);
    if (!at.has_value()) {
      return std::unexpected(at.error());
    }
    const auto checked = checked_word(compute_kind::checked_index, std::move(*at),
                                      std::move(found.length), index.span);
    const auto moved = new_temp(found.pointer, index.span);
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = whole(moved);
    op.computation = compute_kind::pointer_offset;
    op.stride = found.stride;
    op.operands.push_back(std::move(found.data));
    op.operands.push_back(read_of(whole(checked), false));
    emit(std::move(op), index.span);
    return std::optional<place>{project(
        whole(moved), projection{.kind = projection_kind::deref}, found.element)};
  }

  /// `xs[a..b]` (or `a..=b`) into `dest`, a `str` or `slice`: `b <= len` and
  /// then `a <= b` are checked, in that order, and the view's words are
  /// `b - a` and the data pointer advanced by `a`.
  [[nodiscard]] auto lower_range_index(const hir_index &index,
                                       const hir_binary &range,
                                       const place &dest) -> result {
    if (range.lhs == nullptr || range.rhs == nullptr) {
      return refuse(index.span, "a range index without both bounds");
    }
    auto object = container_place(*index.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto view = view_of(*object, index.span);
    if (!view.has_value()) {
      return std::unexpected(view.error());
    }
    if (!view->has_value()) {
      return refuse(index.span, "a range index into `" +
                                    types().display(object->type) + "`");
    }
    auto &found = **view;
    auto start = lower_operand(*range.lhs, !is_pure(*range.rhs));
    if (!start.has_value()) {
      return std::unexpected(start.error());
    }
    auto end = lower_operand(*range.rhs, false);
    if (!end.has_value()) {
      return std::unexpected(end.error());
    }
    const auto usize = types().usize_type();
    if (range.op == ast::binary_op::range_inclusive) {
      const auto past = new_temp(usize, index.span);
      auto add = storage_op{.kind = op_kind::compute};
      add.dest = whole(past);
      add.computation = compute_kind::binary;
      add.binary = ast::binary_op::add;
      add.operands.push_back(std::move(*end));
      add.operands.push_back(word_constant(1));
      emit(std::move(add), index.span);
      end = read_of(whole(past), false);
    }
    const auto last = checked_word(compute_kind::checked_end, std::move(*end),
                                   std::move(found.length), index.span);
    const auto first =
        checked_word(compute_kind::checked_end, std::move(*start),
                     read_of(whole(last), false), index.span);
    const auto &dest_entry = types().entry(types().strip_refinement(dest.type));
    const auto data_type =
        env_.checked.types.ptr_to(found.element, dest_entry.name == "slice_mut");
    auto data = storage_op{.kind = op_kind::compute};
    data.dest = descriptor_part(dest, projection_kind::data, data_type);
    data.computation = compute_kind::pointer_offset;
    data.stride = found.stride;
    data.operands.push_back(std::move(found.data));
    data.operands.push_back(read_of(whole(first), false));
    emit(std::move(data), index.span);
    auto length = storage_op{.kind = op_kind::compute};
    length.dest = descriptor_part(dest, projection_kind::length, usize);
    length.computation = compute_kind::binary;
    length.binary = ast::binary_op::sub;
    length.operands.push_back(read_of(whole(last), false));
    length.operands.push_back(read_of(whole(first), false));
    emit(std::move(length), index.span);
    return {};
  }

  /// Whether `index` indexes a raw pointer.
  [[nodiscard]] auto indexes_raw_pointer(const hir_index &index) const -> bool {
    return types()
               .entry(types().strip_refinement(index.object->type))
               .kind == semantic::type_kind::ptr_kind;
  }

  /// `p[i]` on a raw pointer: the slot `i` elements past `p`, reached by
  /// advancing a copy of the pointer and dereferencing it. The element is
  /// stored inline at its stride, whatever its type. Any other `x[i]` is not
  /// a place the form names yet.
  [[nodiscard]] auto lower_raw_index(const hir_index &index)
      -> std::expected<std::optional<place>, storage_lower_error> {
    if (!indexes_raw_pointer(index)) {
      return std::optional<place>{};
    }
    const auto &pointer_type = index.object->type;
    const auto &entry = types().entry(types().strip_refinement(pointer_type));
    const auto size = env_.layouts.size_of(entry.result);
    if (!size.has_value()) {
      return layout_failure(index.span, size.error());
    }
    auto base = lower_operand(*index.object, !is_pure(*index.index));
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    auto count = lower_operand(*index.index, false);
    if (!count.has_value()) {
      return std::unexpected(count.error());
    }
    const auto moved = new_temp(pointer_type, index.span);
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = whole(moved);
    op.computation = compute_kind::pointer_offset;
    op.stride = *size;
    op.operands.push_back(std::move(*base));
    op.operands.push_back(std::move(*count));
    emit(std::move(op), index.span);
    return std::optional<place>{project(
        whole(moved), projection{.kind = projection_kind::deref}, entry.result)};
  }

  // ------------------------------------------------------------------
  //  Transfers and operands
  // ------------------------------------------------------------------

  /// Initializes `dest` from the value at `source`: a scalar is copied by
  /// value, anything else is copied or moved by its `copy` classification.
  /// The variable `where` read as `expr`'s type. `ptr_cast` retypes a raw
  /// pointer without a node of its own, so a variable read through one can
  /// claim another pointer type; the read copies it into a temporary of that
  /// type, since every raw pointer has the same representation.
  [[nodiscard]] auto retyped(place where, const hir_expr &expr)
      -> std::expected<std::optional<place>, storage_lower_error> {
    const auto &types = env_.checked.types;
    const auto is_pointer = [&](type_id id) {
      return types.entry(types.strip_refinement(id)).kind ==
             semantic::type_kind::ptr_kind;
    };
    if (where.type == expr.type || !is_pointer(where.type) ||
        !is_pointer(expr.type)) {
      return std::optional<place>{std::move(where)};
    }
    const auto temp = new_temp(expr.type, expr.span);
    if (auto ok = transfer(where, whole(temp), expr.span); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    return std::optional<place>{whole(temp)};
  }

  [[nodiscard]] auto transfer(const place &source, const place &dest,
                              const source_span &span) -> result {
    const auto cls = class_of(dest.type, span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    auto op = storage_op{};
    op.dest = dest;
    switch (*cls) {
    case pass_class::zero_sized:
      op.kind = op_kind::init_empty;
      break;
    case pass_class::scalar:
    case pass_class::address:
      op.kind = op_kind::compute;
      op.computation = compute_kind::copy;
      op.operands.push_back(read_of(source, false));
      break;
    case pass_class::handle:
    case pass_class::descriptor:
    case pass_class::aggregate:
      op.kind = env_.is_copy(dest.type) ? op_kind::copy_into
                                        : op_kind::move_into;
      op.source = source;
      break;
    }
    emit(std::move(op), span);
    return {};
  }

  [[nodiscard]] auto read_of(const place &where, bool consume) const -> operand {
    return operand{.kind = operand_kind::read,
                   .where = where,
                   .consume = consume,
                   .type = where.type};
  }

  /// Whether reading a value of this class takes it from its owner.
  [[nodiscard]] auto consumes(pass_class cls, type_id type) const -> bool {
    return cls == pass_class::handle && !env_.is_copy(type);
  }

  /// The scalar-class value of `expr` as an operand. When a later sibling
  /// can have effects, a value read from a place is copied first so the read
  /// happens in source order.
  /// With `borrowed` the callee only looks at a handle, so reading it takes
  /// nothing from its owner.
  [[nodiscard]] auto lower_operand(const hir_expr &expr, bool later_effects,
                                   bool borrowed = false)
      -> std::expected<operand, storage_lower_error> {
    if (const auto owned = owned_receiver_slot(expr)) {
      return operand{.kind = operand_kind::address_of,
                     .where = whole(*owned),
                     .type = expr.type};
    }
    const auto cls = class_of(expr.type, expr.span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    if (expr.kind == hir_node_kind::hir_literal &&
        (*cls == pass_class::scalar || *cls == pass_class::address)) {
      const auto &literal = as<hir_literal>(expr);
      auto text = literal_text(literal);
      if (!text.has_value()) {
        return std::unexpected(text.error());
      }
      return operand{.kind = operand_kind::constant,
                     .type = expr.type,
                     .literal = literal.lit_kind,
                     .text = std::move(*text)};
    }
    if (expr.kind == hir_node_kind::hir_unary &&
        (as<hir_unary>(expr).op == ast::unary_op::addr_of ||
         as<hir_unary>(expr).op == ast::unary_op::addr_of_mut)) {
      const auto &inner = *as<hir_unary>(expr).operand;
      auto target = lower_place(inner);
      if (!target.has_value()) {
        return std::unexpected(target.error());
      }
      auto where = place{};
      if (target->has_value()) {
        where = std::move(**target);
      } else {
        const auto temp = new_temp(inner.type, inner.span);
        where = whole(temp);
        if (auto ok = lower_into(inner, where); !ok.has_value()) {
          return std::unexpected(ok.error());
        }
      }
      return operand{.kind = operand_kind::address_of,
                     .where = std::move(where),
                     .type = expr.type};
    }
    auto source = lower_place(expr);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    if (source->has_value()) {
      const auto consume = !borrowed && consumes(*cls, expr.type);
      // A handle that is only looked at keeps its place: a later argument
      // cannot change which task or crew a variable names, and a copy would
      // have to take the handle from its owner.
      if (!later_effects || borrowed) {
        return read_of(**source, consume);
      }
      const auto temp = new_temp(expr.type, expr.span);
      if (auto ok = transfer(**source, whole(temp), expr.span); !ok.has_value()) {
        return std::unexpected(ok.error());
      }
      if (consume) {
        consumed(temp);
      }
      return read_of(whole(temp), consume);
    }
    const auto temp = new_temp(expr.type, expr.span);
    if (auto ok = lower_into(expr, whole(temp)); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    const auto consume = !borrowed && consumes(*cls, expr.type);
    if (consume) {
      consumed(temp);
    }
    return read_of(whole(temp), consume);
  }

  // ------------------------------------------------------------------
  //  Expressions
  // ------------------------------------------------------------------

  /// Initializes the uninitialized `dest` with the value of `expr`.
  [[nodiscard]] auto lower_into(const hir_expr &expr, const place &dest)
      -> result {
    if (is_never(expr.type) && !is_never(dest.type)) {
      // A branch that ends in a call that does not return (`panic`) never
      // writes the value the other branches do.
      return lower_discarded(expr);
    }
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return lower_literal(as<hir_literal>(expr), dest);
    case hir_node_kind::hir_local_ref:
      if (owned_receiver_slot(expr).has_value()) {
        return lower_receiver_address(expr, dest);
      }
      return transfer_from_place(expr, dest);
    case hir_node_kind::hir_global_ref:
    case hir_node_kind::hir_field:
    case hir_node_kind::hir_tuple_index:
    case hir_node_kind::hir_variant_payload:
      return transfer_from_place(expr, dest);
    case hir_node_kind::hir_match:
      return lower_match(as<hir_match>(expr), dest);
    case hir_node_kind::hir_index: {
      const auto &index = as<hir_index>(expr);
      if (const auto *range = range_of(index); range != nullptr) {
        return lower_range_index(index, *range, dest);
      }
      auto where = lower_place(expr);
      if (!where.has_value()) {
        return std::unexpected(where.error());
      }
      if (!where->has_value()) {
        return refuse(expr.span,
                      "indexing `" +
                          types().display(referent_type(index.object->type)) +
                          "`, which is not a raw pointer, an array, a `str`, "
                          "or a `slice`");
      }
      return transfer(**where, dest, expr.span);
    }
    case hir_node_kind::hir_unary:
      return lower_unary(as<hir_unary>(expr), dest);
    case hir_node_kind::hir_binary:
      return lower_binary(as<hir_binary>(expr), dest);
    case hir_node_kind::hir_cast:
      return lower_cast(as<hir_cast>(expr), dest);
    case hir_node_kind::hir_call:
      if (env_.cell_reads_as_reference(expr.type, dest.type)) {
        // A `cell[T]` result read as a `&T`: the same address, retyped.
        const auto temp = new_temp(expr.type, expr.span);
        if (auto ok = lower_call(as<hir_call>(expr), whole(temp));
            !ok.has_value() || terminated_) {
          return ok;
        }
        auto op = storage_op{.kind = op_kind::compute};
        op.dest = dest;
        op.computation = compute_kind::copy;
        op.operands.push_back(read_of(whole(temp), false));
        emit(std::move(op), expr.span);
        return {};
      }
      return lower_call(as<hir_call>(expr), dest);
    case hir_node_kind::hir_tuple:
      return lower_tuple(as<hir_tuple>(expr), dest);
    case hir_node_kind::hir_struct_init:
      return lower_struct(as<hir_struct_init>(expr), dest);
    case hir_node_kind::hir_array_init:
      return lower_array(as<hir_array_init>(expr), dest);
    case hir_node_kind::hir_variant_init:
      return lower_variant(as<hir_variant_init>(expr), dest);
    case hir_node_kind::hir_block:
      // The statements inside end their own temporaries; those of the
      // enclosing expression, `dest` among them, outlive the block.
      return in_region(expr.span, [&] { return lower_block(as<hir_block>(expr), dest); });
    case hir_node_kind::hir_if:
      return lower_if(as<hir_if>(expr), dest);
    case hir_node_kind::hir_lambda:
      return lower_lambda(as<hir_lambda>(expr), dest);
    case hir_node_kind::hir_closure_drop:
      return lower_closure_drop(as<hir_closure_drop>(expr), dest);
    case hir_node_kind::hir_generator_next:
      return lower_generator_next(as<hir_generator_next>(expr), dest);
    case hir_node_kind::hir_coroutine_cancelled:
      return lower_coroutine_cancelled(expr, dest);
    case hir_node_kind::hir_cell_set:
      return lower_cell_set(as<hir_cell_set>(expr), dest);
    case hir_node_kind::hir_coroutine_cancel:
      return lower_coroutine_cancel(as<hir_coroutine_cancel>(expr), dest);
    case hir_node_kind::hir_container_len:
      return lower_container_len(as<hir_container_len>(expr), dest);
    case hir_node_kind::hir_container_data:
      return lower_container_data(as<hir_container_data>(expr), dest);
    case hir_node_kind::hir_stack_buffer:
      emit(storage_op{.kind = op_kind::zero_fill, .dest = dest}, expr.span);
      return {};
    case hir_node_kind::hir_slice_from_raw_parts:
      return lower_slice_from_raw_parts(as<hir_slice_from_raw_parts>(expr),
                                        dest);
    default:
      return refuse(expr.span, describe(expr.kind) +
                                   " is not part of storage lowering yet");
    }
  }

  [[nodiscard]] auto transfer_from_place(const hir_expr &expr, const place &dest)
      -> result {
    auto source = lower_place(expr);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    if (!source->has_value()) {
      if (expr.kind == hir_node_kind::hir_local_ref) {
        return lower_function_value(as<hir_local_ref>(expr), dest);
      }
      return refuse(expr.span, "a function name used as a value");
    }
    return transfer(**source, dest, expr.span);
  }

  /// A named function used as a value: a closure with an empty environment
  /// whose entry forwards its arguments to the function.
  [[nodiscard]] auto lower_function_value(const hir_local_ref &ref,
                                          const place &dest) -> result {
    const auto &fn_type = types().entry(types().strip_refinement(ref.type));
    if (fn_type.kind != semantic::type_kind::fn_kind) {
      return refuse(ref.span, "a function name used as a value");
    }
    const auto key = ref.owner_module.value_or("") + "::" + ref.name;
    auto entry = std::string{};
    if (const auto found = closures_.forwarders.find(key);
        found != closures_.forwarders.end()) {
      entry = found->second;
    } else {
      entry = out_.name + "$fn" + std::to_string(closure_count_++);
      auto &params = closures_.forwarder_params.emplace_back();
      auto args = ptr_vec<hir_expr>{};
      for (size_t i = 0; i < fn_type.args.size(); ++i) {
        // The forwarder's own symbols; it names nothing else but the callee.
        const auto symbol = static_cast<symbol_id>(k_invalid_symbol_id - 1 - i);
        const auto name = "p" + std::to_string(i);
        params.push_back(
            hir_param{.symbol = symbol, .name = name, .type = fn_type.args[i]});
        args.push_back(std::make_unique<hir_local_ref>(ref.span, fn_type.args[i],
                                                       symbol, name));
      }
      auto callee = std::make_unique<hir_local_ref>(ref.span, ref.type, ref.symbol,
                                                    ref.name, ref.owner_module);
      auto stmts = ptr_vec<hir_node>{};
      stmts.push_back(std::make_unique<hir_return>(
          ref.span, std::make_unique<hir_call>(ref.span, fn_type.result,
                                               std::move(callee), std::move(args))));
      auto body = std::make_unique<hir_block>(ref.span, fn_type.result,
                                              std::move(stmts));
      static const auto no_environment = environment_plan{};
      auto lowered = function_lowerer(body_shape{.name = entry,
                                                 .params = &params,
                                                 .return_type = fn_type.result,
                                                 .body = body.get(),
                                                 .span = ref.span,
                                                 .environment = &no_environment},
                                      env_, closures_)
                         .run();
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      closures_.functions.push_back(std::move(*lowered));
      closures_.forwarder_bodies.push_back(std::move(body));
      closures_.forwarders.emplace(key, entry);
    }
    auto op = storage_op{.kind = op_kind::make_closure};
    op.dest = dest;
    op.entry = entry;
    op.env_size = 0;
    op.env_align = 1;
    emit(std::move(op), ref.span);
    return {};
  }

  /// The container `expr` names, behind any references. A computed one is
  /// built in a temporary first.
  [[nodiscard]] auto container_place(const hir_expr &expr)
      -> std::expected<place, storage_lower_error> {
    auto found = lower_place(expr);
    if (!found.has_value()) {
      return std::unexpected(found.error());
    }
    if (found->has_value()) {
      return auto_deref(std::move(**found));
    }
    const auto temp = new_temp(expr.type, expr.span);
    if (auto ok = lower_into(expr, whole(temp)); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    return auto_deref(whole(temp));
  }

  [[nodiscard]] static auto descriptor_part(place descriptor,
                                            projection_kind kind,
                                            type_id type) -> place {
    descriptor.steps.push_back(projection{.kind = kind, .type = type});
    descriptor.type = type;
    return descriptor;
  }

  /// `xs.len()` as `for`-loop lowering asks for it: the length word of a
  /// `str` or `slice`, or the count of an array.
  [[nodiscard]] auto lower_container_len(const hir_container_len &node,
                                         const place &dest) -> result {
    auto object = container_place(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto layout = layout_of(object->type, node.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    if ((*layout)->kind == runtime::value_layout_kind::descriptor) {
      return transfer(descriptor_part(std::move(*object), projection_kind::length,
                                      node.type),
                      dest, node.span);
    }
    if ((*layout)->kind == runtime::value_layout_kind::array) {
      auto op = storage_op{.kind = op_kind::compute};
      op.dest = dest;
      op.computation = compute_kind::constant;
      op.literal = token_kind::int_lit;
      op.text = std::to_string((*layout)->count);
      emit(std::move(op), node.span);
      return {};
    }
    return refuse(node.span,
                  "the length of `" + types().display(object->type) + "`");
  }

  /// `xs.as_ptr()`: the data word of a `str` or `slice`, or the address of
  /// an array, which is that of its first element. An empty container's
  /// data word is returned as it is, without a bounds check; an empty
  /// array's address is its zero-sized slot's.
  /// The parameter slot of the receiver this function owns, when `expr` names
  /// it (`self`, typed `&T` in the HIR, is a `T` here).
  [[nodiscard]] auto owned_receiver_slot(const hir_expr &expr) const
      -> std::optional<storage_id> {
    if (!owned_receiver_.has_value() || expr.kind != hir_node_kind::hir_local_ref ||
        as<hir_local_ref>(expr).symbol != owned_receiver_->symbol) {
      return std::nullopt;
    }
    return owned_receiver_->slot;
  }

  /// `self` as a value, in a function that owns its receiver: the address of
  /// the parameter.
  [[nodiscard]] auto lower_receiver_address(const hir_expr &expr,
                                            const place &dest) -> result {
    auto value = lower_operand(expr, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = dest;
    op.computation = compute_kind::copy;
    op.operands.push_back(std::move(*value));
    emit(std::move(op), expr.span);
    return {};
  }

  [[nodiscard]] auto lower_container_data(const hir_container_data &node,
                                          const place &dest) -> result {
    auto object = container_place(*node.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    const auto layout = layout_of(object->type, node.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind == runtime::value_layout_kind::descriptor) {
      return transfer(descriptor_part(std::move(*object), projection_kind::data,
                                      node.type),
                      dest, node.span);
    }
    if (found.kind == runtime::value_layout_kind::array) {
      const auto array = *object;
      auto op = storage_op{.kind = op_kind::compute};
      op.dest = dest;
      op.computation = compute_kind::copy;
      op.operands.push_back(operand{.kind = operand_kind::address_of,
                                    .where = std::move(*object),
                                    .type = node.type});
      emit(std::move(op), node.span);
      if (node.takes_elements) {
        emit(storage_op{.kind = op_kind::forget, .dest = array}, node.span);
      }
      return {};
    }
    return refuse(node.span, "the data address of `" +
                                 types().display(object->type) + "`");
  }

  /// `slice_from_raw_parts(p, n)`: a descriptor whose words are `n` and `p`,
  /// evaluated in that source order (`p` first).
  [[nodiscard]] auto lower_slice_from_raw_parts(
      const hir_slice_from_raw_parts &node, const place &dest) -> result {
    auto pointer = lower_operand(*node.pointer, !is_pure(*node.len));
    if (!pointer.has_value()) {
      return std::unexpected(pointer.error());
    }
    auto count = lower_operand(*node.len, false);
    if (!count.has_value()) {
      return std::unexpected(count.error());
    }
    auto write = [&](projection_kind kind, operand value, type_id type) {
      auto op = storage_op{.kind = op_kind::compute};
      op.dest = descriptor_part(dest, kind, type);
      op.computation = compute_kind::copy;
      op.operands.push_back(std::move(value));
      emit(std::move(op), node.span);
    };
    write(projection_kind::length, std::move(*count), node.len->type);
    write(projection_kind::data, std::move(*pointer), node.pointer->type);
    return {};
  }

  /// The spelling of a literal. A `size_of`/`align_of` answer is asked again
  /// of the concrete layouts: HIR's own answer is for the legacy boxed
  /// aggregates, and a slot here is as wide as its inline layout.
  [[nodiscard]] auto literal_text(const hir_literal &literal)
      -> std::expected<std::string, storage_lower_error> {
    if (!literal.question.has_value()) {
      return literal.value;
    }
    const auto &question = *literal.question;
    const auto answer = question.wants_alignment
                            ? env_.layouts.align_of(question.type)
                            : env_.layouts.size_of(question.type);
    if (!answer.has_value()) {
      return layout_failure(literal.span, answer.error());
    }
    return std::to_string(*answer);
  }

  [[nodiscard]] auto lower_literal(const hir_literal &literal, const place &dest)
      -> result {
    const auto cls = class_of(dest.type, literal.span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    auto op = storage_op{};
    op.dest = dest;
    if (*cls == pass_class::zero_sized) {
      op.kind = op_kind::init_empty;
    } else if (*cls == pass_class::scalar || *cls == pass_class::address) {
      op.kind = op_kind::compute;
      op.computation = compute_kind::constant;
      op.literal = literal.lit_kind;
      auto text = literal_text(literal);
      if (!text.has_value()) {
        return std::unexpected(text.error());
      }
      op.text = std::move(*text);
    } else if (literal.lit_kind == token_kind::string_lit &&
               types().entry(types().strip_refinement(dest.type)).name == "str") {
      // The bytes are static data; the descriptor built here points at them.
      auto text = decode_string_literal(literal.value);
      if (!text.has_value()) {
        return malformed(literal.span, "a string literal that does not decode");
      }
      op.kind = op_kind::string_literal;
      op.text = std::move(*text);
    } else {
      return refuse(literal.span,
                    "a literal of type `" + types().display(dest.type) +
                        "`, which needs static data");
    }
    emit(std::move(op), literal.span);
    return {};
  }

  [[nodiscard]] auto require_scalar_dest(const place &dest,
                                         const source_span &span) -> result {
    const auto cls = class_of(dest.type, span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    if (*cls != pass_class::scalar && *cls != pass_class::address) {
      return refuse(span, "a scalar operator producing `" +
                              types().display(dest.type) + "`");
    }
    return {};
  }

  [[nodiscard]] auto lower_unary(const hir_unary &unary, const place &dest)
      -> result {
    if (unary.op == ast::unary_op::deref) {
      return transfer_from_place(unary, dest);
    }
    if (auto ok = require_scalar_dest(dest, unary.span); !ok.has_value()) {
      return ok;
    }
    const auto takes_address = unary.op == ast::unary_op::addr_of ||
                               unary.op == ast::unary_op::addr_of_mut;
    // `&x` is the address of `x`, which `lower_operand` builds from the whole
    // expression; any other operator reads its operand's value.
    auto value = lower_operand(takes_address ? static_cast<const hir_expr &>(unary)
                                                     : *unary.operand,
                               false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = dest;
    if (takes_address) {
      op.computation = compute_kind::copy;
    } else {
      op.computation = compute_kind::unary;
      op.unary = unary.op;
    }
    op.operands.push_back(std::move(*value));
    emit(std::move(op), unary.span);
    return {};
  }

  [[nodiscard]] auto lower_binary(const hir_binary &binary, const place &dest)
      -> result {
    if (auto ok = require_scalar_dest(dest, binary.span); !ok.has_value()) {
      return ok;
    }
    if (binary.op == ast::binary_op::logical_and ||
        binary.op == ast::binary_op::logical_or) {
      return lower_short_circuit(binary, dest);
    }
    auto lhs = lower_operand(*binary.lhs, !is_pure(*binary.rhs));
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = lower_operand(*binary.rhs, false);
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = dest;
    op.computation = compute_kind::binary;
    op.binary = binary.op;
    op.operands.push_back(std::move(*lhs));
    op.operands.push_back(std::move(*rhs));
    emit(std::move(op), binary.span);
    return {};
  }

  /// `a && b` and `a || b`: the left side is built in `dest`, and the right
  /// side runs, over it, only when the left side does not decide the result.
  /// The right side may have an effect or panic, so it must not run eagerly.
  [[nodiscard]] auto lower_short_circuit(const hir_binary &binary,
                                         const place &dest) -> result {
    const auto is_and = binary.op == ast::binary_op::logical_and;
    auto left = in_region(binary.lhs->span,
                          [&] { return lower_into(*binary.lhs, dest); });
    if (!left.has_value()) {
      return left;
    }
    auto head = storage_op{.kind = op_kind::if_begin};
    head.condition = read_of(dest, false);
    emit(std::move(head), binary.span);
    if (!is_and) {
      emit(storage_op{.kind = op_kind::else_begin}, binary.span);
    }
    auto right = in_region(binary.rhs->span,
                           [&] { return lower_into(*binary.rhs, dest); });
    if (!right.has_value()) {
      return right;
    }
    emit(storage_op{.kind = op_kind::if_end}, binary.span);
    terminated_ = false;
    return {};
  }

  [[nodiscard]] auto lower_cast(const hir_cast &cast, const place &dest)
      -> result {
    if (auto ok = require_scalar_dest(dest, cast.span); !ok.has_value()) {
      return ok;
    }
    auto value = lower_operand(*cast.operand, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = dest;
    op.computation = compute_kind::cast;
    op.operands.push_back(std::move(*value));
    emit(std::move(op), cast.span);
    return {};
  }

  // ------------------------------------------------------------------
  //  Constructors
  // ------------------------------------------------------------------

  [[nodiscard]] auto empty_value(const place &dest, const source_span &span)
      -> result {
    auto op = storage_op{.kind = op_kind::init_empty};
    op.dest = dest;
    emit(std::move(op), span);
    return {};
  }

  /// A finished part of a value still under construction: if the construction
  /// is left early, the part is dropped, since nothing else owns it yet.
  auto note_built(const place &part) -> void {
    if (env_.needs_drop(part.type)) {
      partials_.push_back(part);
    }
  }

  auto forget_built(size_t mark) -> void {
    partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(mark),
                    partials_.end());
  }

  /// Drops the finished parts of every value under construction, newest
  /// first, before the function is left.
  [[nodiscard]] auto drop_partials(const source_span &span) -> result {
    for (auto it = partials_.rbegin(); it != partials_.rend(); ++it) {
      if (auto ok = lower_value_drop(*it, span); !ok.has_value()) {
        return ok;
      }
    }
    return {};
  }

  [[nodiscard]] auto lower_tuple(const hir_tuple &tuple, const place &dest)
      -> result {
    const auto layout = layout_of(dest.type, tuple.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.members.size() != tuple.elements.size() ||
        (found.kind != runtime::value_layout_kind::tuple &&
         !tuple.elements.empty())) {
      return malformed(tuple.span, "a tuple literal does not match `" +
                                       types().display(dest.type) + "`");
    }
    if (tuple.elements.empty()) {
      return empty_value(dest, tuple.span);
    }
    const auto mark = partials_.size();
    for (size_t i = 0; i < tuple.elements.size(); ++i) {
      const auto part =
          project(dest, member_step(i, ""), found.members[i].type);
      if (auto ok = lower_into(*tuple.elements[i], part); !ok.has_value()) {
        return ok;
      }
      note_built(part);
    }
    forget_built(mark);
    return {};
  }

  [[nodiscard]] auto lower_struct(const hir_struct_init &init, const place &dest)
      -> result {
    const auto layout = layout_of(dest.type, init.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind != runtime::value_layout_kind::structure) {
      return malformed(init.span, "a struct literal builds `" +
                                      types().display(dest.type) +
                                      "`, which is not a struct");
    }
    if (init.fields.size() != found.members.size()) {
      return refuse(init.span,
                    "a struct literal that leaves fields of `" +
                        types().display(dest.type) + "` unwritten");
    }
    if (found.members.empty()) {
      return empty_value(dest, init.span);
    }
    const auto mark = partials_.size();
    for (const auto &field : init.fields) {
      auto index = found.members.size();
      for (size_t i = 0; i < found.members.size(); ++i) {
        if (found.members[i].name == field.name) {
          index = i;
        }
      }
      if (index == found.members.size()) {
        return malformed(init.span, "`" + types().display(dest.type) +
                                        "` has no field `" + field.name + "`");
      }
      const auto part =
          project(dest, member_step(index, field.name), found.members[index].type);
      if (auto ok = lower_into(*field.value, part); !ok.has_value()) {
        return ok;
      }
      note_built(part);
    }
    forget_built(mark);
    return {};
  }

  [[nodiscard]] auto lower_array(const hir_array_init &init, const place &dest)
      -> result {
    const auto layout = layout_of(dest.type, init.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (init.fill_value != nullptr) {
      return lower_array_fill(init, found, dest);
    }
    if (found.kind != runtime::value_layout_kind::array ||
        found.count != init.elements.size()) {
      return refuse(init.span, "an array literal that does not build a fixed "
                               "`array` of its own length");
    }
    if (init.elements.empty()) {
      return empty_value(dest, init.span);
    }
    const auto mark = partials_.size();
    for (size_t i = 0; i < init.elements.size(); ++i) {
      const auto part = project(
          dest, projection{.kind = projection_kind::element, .index = i},
          found.element);
      if (auto ok = lower_into(*init.elements[i], part); !ok.has_value()) {
        return ok;
      }
      note_built(part);
    }
    forget_built(mark);
    return {};
  }

  /// `[value; count]`: the value is built once, into element 0, and copied
  /// over the rest. The count is the array type's; the checker made the two
  /// agree. With no elements the value is still evaluated, in a temporary.
  [[nodiscard]] auto lower_array_fill(const hir_array_init &init,
                                      const runtime::value_layout &found,
                                      const place &dest) -> result {
    if (found.kind != runtime::value_layout_kind::array) {
      return refuse(init.span, "an array fill that does not build a fixed "
                               "`array`");
    }
    if (!env_.is_copy(found.element)) {
      return refuse(init.span, "an array fill of `" +
                                   types().display(found.element) +
                                   "`, which is moved, not copied");
    }
    if (found.count == 0) {
      const auto temp = new_temp(found.element, init.span);
      if (auto ok = lower_into(*init.fill_value, whole(temp)); !ok.has_value()) {
        return ok;
      }
      return empty_value(dest, init.span);
    }
    const auto first = project(
        dest, projection{.kind = projection_kind::element, .index = 0},
        found.element);
    if (auto ok = lower_into(*init.fill_value, first); !ok.has_value()) {
      return ok;
    }
    auto op = storage_op{.kind = op_kind::replicate};
    op.dest = dest;
    op.source = first;
    emit(std::move(op), init.span);
    return {};
  }

  [[nodiscard]] auto lower_variant(const hir_variant_init &init, const place &dest)
      -> result {
    const auto layout = layout_of(dest.type, init.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    const auto *variant = found.find_variant(init.variant_name);
    if (found.kind != runtime::value_layout_kind::sum || variant == nullptr ||
        variant->payload.size() != init.args.size()) {
      return malformed(init.span, "`@" + init.variant_name +
                                      "` does not construct `" +
                                      types().display(dest.type) + "`");
    }
    const auto mark = partials_.size();
    for (size_t i = 0; i < init.args.size(); ++i) {
      const auto part = project(
          dest,
          projection{.kind = projection_kind::payload,
                     .index = i,
                     .name = init.variant_name},
          variant->payload[i].type);
      if (auto ok = lower_into(*init.args[i], part); !ok.has_value()) {
        return ok;
      }
      note_built(part);
    }
    forget_built(mark);
    auto op = storage_op{.kind = op_kind::publish_variant};
    op.dest = dest;
    op.variant = init.variant_name;
    emit(std::move(op), init.span);
    return {};
  }

  // ------------------------------------------------------------------
  //  Calls
  // ------------------------------------------------------------------

  /// The expression that is `block`'s value when `stmt` is its last
  /// statement. A block HIR builds ends in the bare expression; a block of
  /// source code with a value ends in an expression statement.
  [[nodiscard]] auto tail_value(const hir_block &block, const hir_node &stmt,
                                bool wanted) const -> const hir_expr * {
    if (stmt.kind != hir_node_kind::hir_expr_stmt) {
      return dynamic_cast<const hir_expr *>(&stmt);
    }
    // An arm of a `match` has no type of its own; its destination says whether
    // the arm's last expression is the value.
    const auto yields = wanted || (block.type != semantic::k_unknown_type &&
                                   !types().is_unit(block.type));
    return yields ? as<hir_expr_stmt>(stmt).expr.get() : nullptr;
  }

  /// A block runs its statements in order. When it ends in an expression,
  /// that expression is the block's value and is built in `dest`. HIR makes a
  /// discarded temporary, or a value computed before scope-exit drops, a
  /// block of this shape.
  [[nodiscard]] auto lower_block(const hir_block &block, const place &dest,
                                 bool wanted = false) -> result {
    for (size_t i = 0; i < block.stmts.size() && !terminated_; ++i) {
      const auto &stmt = *block.stmts[i];
      const auto last = i + 1 == block.stmts.size();
      const auto *value = last ? tail_value(block, stmt, wanted) : nullptr;
      if (value != nullptr) {
        if (auto ok = lower_into(*value, dest); !ok.has_value()) {
          return ok;
        }
        continue;
      }
      if (auto ok = lower_stmt(stmt); !ok.has_value()) {
        return ok;
      }
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Closures
  // ------------------------------------------------------------------

  [[nodiscard]] static auto round_up(size_t value, size_t align) -> size_t {
    return (value + align - 1) / align * align;
  }

  /// Whether every path through `node` leaves the function or the loop, or
  /// does not return, so a value it is typed with is never produced. Such a
  /// trailing `if` or `match` is lowered as the statement it is, with no
  /// temporary for the value.
  [[nodiscard]] auto always_leaves(const hir_node &node) const -> bool {
    switch (node.kind) {
    case hir_node_kind::hir_return:
    case hir_node_kind::hir_break:
    case hir_node_kind::hir_continue:
      return true;
    case hir_node_kind::hir_expr_stmt:
      return always_leaves(*as<hir_expr_stmt>(node).expr);
    case hir_node_kind::hir_block: {
      const auto &block = as<hir_block>(node);
      return !block.stmts.empty() && always_leaves(*block.stmts.back());
    }
    case hir_node_kind::hir_if: {
      const auto &branches = as<hir_if>(node);
      if (branches.else_body == nullptr ||
          !always_leaves(*branches.else_body)) {
        return false;
      }
      for (const auto &branch : branches.branches) {
        if (!always_leaves(*branch.body)) {
          return false;
        }
      }
      return true;
    }
    case hir_node_kind::hir_match: {
      for (const auto &arm : as<hir_match>(node).arms) {
        if (!always_leaves(*arm.body)) {
          return false;
        }
      }
      return true;
    }
    default: {
      const auto *expr = dynamic_cast<const hir_expr *>(&node);
      return expr != nullptr && is_never(expr->type);
    }
    }
  }

  /// Whether `id` is `never`: an expression of that type does not produce a
  /// value, because evaluating it does not return.
  [[nodiscard]] auto is_never(type_id id) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(id));
    return entry.kind == semantic::type_kind::builtin_kind &&
           entry.name == "never";
  }

  /// A `unit` type id, for a function the lowering synthesizes.
  [[nodiscard]] auto unit_type() const -> type_id {
    for (size_t id = 0; id < types().count(); ++id) {
      if (types().is_unit(static_cast<type_id>(id))) {
        return static_cast<type_id>(id);
      }
    }
    return semantic::k_unknown_type;
  }

  /// Lays out the environment of `lambda`: each captured local of the
  /// enclosing function in capture order, an owned or copied value inline at
  /// its own alignment, a by-reference capture as one address. A captured name
  /// that is not a local (a module-level function) needs no slot.
  [[nodiscard]] auto plan_environment(const hir_lambda &lambda)
      -> std::expected<environment_plan, storage_lower_error> {
    auto owned = std::unordered_set<symbol_id>{};
    if (lambda.drop_glue != nullptr) {
      for (const auto symbol : free_variables(*lambda.drop_glue)) {
        owned.insert(symbol);
      }
    }
    auto plan = environment_plan{};
    auto running = size_t{0};
    for (const auto &capture : capture_plan(lambda)) {
      const auto found = locals_.find(capture.symbol);
      if (found == locals_.end()) {
        continue;
      }
      const auto &slot = out_.slots[found->second];
      auto entry = environment_entry{
          .symbol = capture.symbol, .type = slot.type, .name = slot.name};
      auto size = runtime::k_word_size;
      auto align = runtime::k_word_size;
      if (ast::is_reference_capture(capture.mode)) {
        entry.kind = capture_kind::reference;
      } else {
        const auto layout = layout_of(slot.type, lambda.span);
        if (!layout.has_value()) {
          return std::unexpected(layout.error());
        }
        size = (*layout)->size;
        align = (*layout)->align;
        if (size == 0) {
          return refuse(lambda.span,
                        "a closure that captures a zero-sized value");
        }
        entry.kind = owned.contains(capture.symbol) ? capture_kind::move
                                                    : capture_kind::copy;
      }
      entry.offset = round_up(running, align);
      running = entry.offset + size;
      plan.align = std::max(plan.align, align);
      plan.entries.push_back(std::move(entry));
    }
    plan.size = round_up(running, plan.align);
    return plan;
  }

  /// Lowers the closure `lambda` into `dest`: its body and drop glue become
  /// functions of the module taking the environment as a hidden operand, and
  /// the value is built by `make_closure`.
  [[nodiscard]] auto lower_lambda(const hir_lambda &lambda, const place &dest)
      -> result {
    auto plan = plan_environment(lambda);
    if (!plan.has_value()) {
      return std::unexpected(plan.error());
    }
    const auto name = out_.name + "$closure" + std::to_string(closure_count_++);
    auto body = function_lowerer(
                    body_shape{.name = name,
                               .params = &lambda.params,
                               .return_type = lambda.return_type,
                               .body = lambda.body.get(),
                               .span = lambda.span,
                               .environment = &*plan},
                    env_, closures_)
                    .run();
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    closures_.functions.push_back(std::move(*body));

    auto glue_name = std::string{};
    if (plan->size != 0) {
      // Whatever the environment owns is dropped, and the block is freed. A
      // closure whose environment is all copies still has the block to free.
      static const auto no_params = std::vector<hir_param>{};
      glue_name = name + "$drop";
      auto glue = function_lowerer(
                      body_shape{.name = glue_name,
                                 .params = &no_params,
                                 .return_type = unit_type(),
                                 .body = lambda.drop_glue != nullptr
                                             ? lambda.drop_glue->body.get()
                                             : nullptr,
                                 .span = lambda.span,
                                 .environment = &*plan,
                                 .releases_environment = true},
                      env_, closures_)
                      .run();
      if (!glue.has_value()) {
        return std::unexpected(glue.error());
      }
      closures_.functions.push_back(std::move(*glue));
    }

    auto op = storage_op{.kind = op_kind::make_closure};
    op.dest = dest;
    op.entry = name;
    op.glue = glue_name;
    op.env_size = plan->size;
    op.env_align = plan->align;
    for (const auto &entry : plan->entries) {
      const auto source = locals_.at(entry.symbol);
      if (entry.kind == capture_kind::move &&
          out_.slots[source].cls == storage_class::closure_environment) {
        return refuse(lambda.span,
                      "a closure that takes ownership of a value captured by "
                      "an enclosing closure");
      }
      op.captures.push_back(closure_capture{
          .source = whole(source), .kind = entry.kind, .offset = entry.offset});
    }
    emit(std::move(op), lambda.span);
    return {};
  }

  /// `hir_closure_drop`: runs the closure's drop glue, which drops what the
  /// environment owns and frees the block. The value is then dead.
  [[nodiscard]] auto lower_closure_drop(const hir_closure_drop &drop,
                                        const std::optional<place> &dest)
      -> result {
    auto where = lower_place(*drop.object);
    if (!where.has_value()) {
      return std::unexpected(where.error());
    }
    if (!where->has_value()) {
      return refuse(drop.span, "the drop of a closure that is not a variable");
    }
    auto op = storage_op{.kind = op_kind::drop_in_place};
    op.dest = std::move(**where);
    emit(std::move(op), drop.span);
    if (dest.has_value()) {
      emit(storage_op{.kind = op_kind::init_empty, .dest = *dest}, drop.span);
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Generators
  // ------------------------------------------------------------------

  /// A generator's step. Its parameters and every local are slots of the state
  /// block the constructor filled, so a suspension saves nothing: the function
  /// returns, and the next call resumes at the point the dispatch names.
  [[nodiscard]] auto run_step() -> std::expected<storage_function, storage_lower_error> {
    out_.name = fn_.name;
    out_.coroutine_step = true;
    const auto word = types().usize_type();
    auto signature = classify_signature(
        env_.layouts, std::vector<type_id>{word, fn_.generator}, word);
    if (!signature.has_value()) {
      return layout_failure(fn_.span, signature.error());
    }
    signature->takes_environment = true;
    out_.signature = std::move(*signature);
    const auto resume = new_slot(storage_class::parameter, word, "resume",
                                 k_invalid_symbol_id, fn_.span, true);
    object_slot_ = new_slot(storage_class::parameter, fn_.generator, "coroutine",
                            k_invalid_symbol_id, fn_.span, true);
    out_.param_slots = {resume, object_slot_};
    for (const auto &param : *fn_.params) {
      const auto id = new_slot(storage_class::coroutine_frame, param.type,
                               param.name, param.symbol, fn_.span, true);
      out_.slots[id].owns_nothing = param.lent && !env_.is_copy(param.type);
      locals_[param.symbol] = id;
    }
    emit(storage_op{.kind = op_kind::coroutine_dispatch, .source = whole(resume)},
         fn_.span);
    if (fn_.body != nullptr) {
      for (const auto &stmt : fn_.body->stmts) {
        if (auto ok = lower_stmt(*stmt); !ok.has_value()) {
          return std::unexpected(ok.error());
        }
        if (terminated_) {
          break;
        }
      }
    }
    if (!terminated_) {
      if (auto ended = flush_pending(fn_.span); !ended.has_value()) {
        return std::unexpected(ended.error());
      }
      emit_return(std::nullopt, fn_.span);
    }
    out_.ops.front().depth = suspensions_;
    // Lay the state block out: parameters first, in order, then the rest.
    auto running = size_t{0};
    for (auto &slot : out_.slots) {
      if (slot.cls != storage_class::coroutine_frame) {
        continue;
      }
      const auto layout = layout_of(slot.type, slot.span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      slot.env_offset = round_up(running, (*layout)->align);
      running = slot.env_offset + (*layout)->size;
      state_align_ = std::max(state_align_, (*layout)->align);
    }
    state_size_ = round_up(running, state_align_);
    return std::move(out_);
  }

  /// `yield value`: the value is built in a slot of the state block, then
  /// handed to the resumer, which moves it out when it reads the item.
  [[nodiscard]] auto lower_yield(const hir_yield &yield) -> result {
    if (!fn_.step) {
      return refuse(yield.span, "`yield` outside the body of a generator");
    }
    if (yield.awaits) {
      return lower_await_point(yield);
    }
    // A task hands over its value, or `result[value, error]` when it can
    // fail; the failure arm of `?` yields its operand, rebuilt at that type.
    auto item_type = yield.value->type;
    const auto &object_entry =
        types().entry(types().strip_refinement(fn_.generator));
    if (is_task_type(fn_.generator) &&
        object_entry.args.size() >= 2) {
      item_type = types().entry(object_entry.args[1]).name == "never"
                      ? object_entry.args[0]
                      : env_.checked.types.builtin_generic(
                            "result", {object_entry.args[0], object_entry.args[1]});
    }
    const auto item = new_temp(item_type, yield.value->span);
    if (auto ok = lower_returned(*yield.value, whole(item)); !ok.has_value()) {
      return ok;
    }
    auto suspend = storage_op{.kind = op_kind::coroutine_suspend};
    suspend.source = whole(item);
    suspend.dest = whole(object_slot_);
    suspend.depth = ++suspensions_;
    emit(std::move(suspend), yield.span);
    consumed(item);
    return flush_pending(yield.span);
  }

  /// The coroutine `object` names, in place when it is a variable and in a
  /// temporary otherwise.
  [[nodiscard]] auto lower_coroutine_place(const hir_expr &object)
      -> std::expected<place, storage_lower_error> {
    auto found = lower_place(object);
    if (!found.has_value()) {
      return std::unexpected(found.error());
    }
    if (found->has_value()) {
      return auto_deref(std::move(**found));
    }
    const auto temp = new_temp(object.type, object.span);
    if (auto ok = lower_into(object, whole(temp)); !ok.has_value()) {
      return std::unexpected(ok.error());
    }
    return whole(temp);
  }

  /// Whether `type` is a `task`.
  [[nodiscard]] auto is_task_type(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           entry.name == "task";
  }

  /// Whether `type` is a `crew`.
  [[nodiscard]] auto is_crew_type(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    return entry.kind == semantic::type_kind::builtin_generic_kind &&
           entry.name == "crew";
  }

  /// Whether `type` is a tuple of tasks, the group `await par` and `await
  /// race` wait on.
  [[nodiscard]] auto is_task_group_type(type_id type) const -> bool {
    const auto &entry = types().entry(types().strip_refinement(type));
    if (entry.kind != semantic::type_kind::tuple_kind || entry.args.empty()) {
      return false;
    }
    for (const auto member : entry.args) {
      if (!is_task_type(member)) {
        return false;
      }
    }
    return true;
  }

  /// A task's `await` point: the step polls the awaited task and, if it has
  /// not finished, returns `pending`. Woken, it resumes here and polls again.
  /// Past it the awaited task has finished and `lower_task_result` reads it.
  [[nodiscard]] auto lower_await_point(const hir_yield &yield) -> result {
    const auto group = is_task_group_type(yield.value->type);
    const auto crew = is_crew_type(yield.value->type);
    if (!group && !crew &&
        (yield.awaits_all || !is_task_type(yield.value->type))) {
      return refuse(yield.span, "an `await` of anything but one task or a "
                                "group of tasks");
    }
    auto awaited = lower_coroutine_place(*yield.value);
    if (!awaited.has_value()) {
      return std::unexpected(awaited.error());
    }
    auto wait = storage_op{.kind = op_kind::coroutine_await};
    wait.waits = crew     ? wait_mode::crew
                 : !group ? wait_mode::one
                 : yield.awaits_all ? wait_mode::all
                                    : wait_mode::any;
    wait.source = std::move(*awaited);
    wait.dest = whole(object_slot_);
    wait.depth = ++suspensions_;
    emit(std::move(wait), yield.span);
    return flush_pending(yield.span);
  }

  /// `await task`'s read of a finished task's result. A task body has waited
  /// for it already; the entry `async def main` is an ordinary function with
  /// nothing to suspend, so it blocks until the task is done.
  [[nodiscard]] auto lower_task_result(const hir_generator_next &next,
                                       const place &dest) -> result {
    if (!is_task_type(next.object->type)) {
      return malformed(next.span, "an `await` of " +
                                      types().display(next.object->type) +
                                      ", which is not a task");
    }
    auto task = lower_coroutine_place(*next.object);
    if (!task.has_value()) {
      return std::unexpected(task.error());
    }
    if (!fn_.step) {
      const auto word = types().usize_type();
      auto signature = classify_signature(
          env_.layouts, std::vector<type_id>{task->type, word}, unit_type());
      if (!signature.has_value()) {
        return layout_failure(next.span, signature.error());
      }
      auto block = storage_op{.kind = op_kind::call_into};
      block.callee.name = "rt_task_block_on";
      block.args.push_back(call_argument{.source_index = 0,
                                         .cls = signature->params[0].cls,
                                         .value = read_of(*task, false)});
      block.args.push_back(call_argument{
          .source_index = 1,
          .cls = signature->params[1].cls,
          .value = word_constant(
              static_cast<uint64_t>(runtime::scheduler::pool::io))});
      block.signature = std::move(*signature);
      block.dest = whole(new_temp(unit_type(), next.span));
      emit(std::move(block), next.span);
    }
    emit(storage_op{.kind = op_kind::generator_take,
                    .dest = dest,
                    .source = std::move(*task)},
         next.span);
    return {};
  }

  /// `g.next()`: resumes the generator once. While it is suspended the item it
  /// yielded is moved into an `@some`; once it has finished the result is
  /// `@none`.
  [[nodiscard]] auto lower_generator_next(const hir_generator_next &next,
                                          const place &dest) -> result {
    if (next.awaiting) {
      return lower_task_result(next, dest);
    }
    auto found = lower_coroutine_place(*next.object);
    if (!found.has_value()) {
      return std::unexpected(found.error());
    }
    const auto generator = std::move(*found);
    const auto layout = layout_of(dest.type, next.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *some = (*layout)->find_variant("some");
    if (some == nullptr || some->payload.size() != 1 ||
        (*layout)->find_variant("none") == nullptr) {
      return malformed(next.span, "a generator step gives `" +
                                      types().display(dest.type) +
                                      "`, not an option");
    }
    const auto status = new_temp(types().usize_type(), next.span);
    emit(storage_op{.kind = op_kind::generator_resume,
                    .dest = whole(status),
                    .source = generator},
         next.span);
    return branch_on(
        next.span,
        [&](const place &flag) {
          auto op = storage_op{.kind = op_kind::compute};
          op.dest = flag;
          op.computation = compute_kind::binary;
          op.binary = ast::binary_op::eq_eq;
          op.operands.push_back(read_of(whole(status), false));
          op.operands.push_back(word_constant(
              runtime::coroutine::raw(runtime::coroutine::status::suspended)));
          emit(std::move(op), next.span);
        },
        [&]() -> result {
          emit(storage_op{.kind = op_kind::generator_take,
                          .dest = project(dest,
                                          projection{.kind = projection_kind::payload,
                                                     .index = 0,
                                                     .name = "some"},
                                          some->payload[0].type),
                          .source = generator},
               next.span);
          auto op = storage_op{.kind = op_kind::publish_variant};
          op.dest = dest;
          op.variant = "some";
          emit(std::move(op), next.span);
          return {};
        },
        [&]() -> result {
          auto op = storage_op{.kind = op_kind::publish_variant};
          op.dest = dest;
          op.variant = "none";
          emit(std::move(op), next.span);
          return {};
        });
  }

  /// The test the cancellation check HIR places after each `yield`.
  [[nodiscard]] auto lower_coroutine_cancelled(const hir_expr &expr,
                                               const place &dest) -> result {
    if (!fn_.step) {
      return refuse(expr.span, "a cancellation test outside a generator body");
    }
    emit(storage_op{.kind = op_kind::coroutine_cancelled,
                    .dest = dest,
                    .source = whole(object_slot_)},
         expr.span);
    return {};
  }

  /// Dropping a generator: one that has not finished is resumed once to
  /// unwind, and the coroutine is released. The value is then dead.
  [[nodiscard]] auto lower_coroutine_cancel(const hir_coroutine_cancel &cancel,
                                            const std::optional<place> &dest)
      -> result {
    auto where = lower_place(*cancel.object);
    if (!where.has_value()) {
      return std::unexpected(where.error());
    }
    if (!where->has_value()) {
      return refuse(cancel.span, "the drop of a generator that is not a variable");
    }
    if (auto ok = lower_unread_result_drop(**where, cancel.span); !ok.has_value()) {
      return ok;
    }
    auto op = storage_op{.kind = op_kind::drop_in_place};
    op.dest = std::move(**where);
    emit(std::move(op), cancel.span);
    if (dest.has_value()) {
      emit(storage_op{.kind = op_kind::init_empty, .dest = *dest}, cancel.span);
    }
    return {};
  }

  /// A task that finished and was never read still owns its result, so
  /// dropping the handle drops the result first. The handle says whether the
  /// task has finished (it may still be running on a worker, which owns the
  /// result until it stops) and whether an unread item is held. A result that
  /// is read leaves no item, so the common drop after a read does nothing.
  [[nodiscard]] auto lower_unread_result_drop(const place &task,
                                              const source_span &span)
      -> result {
    const auto &entry = types().entry(types().strip_refinement(task.type));
    if (entry.kind != semantic::type_kind::builtin_generic_kind ||
        entry.name != "task" || entry.args.size() < 2) {
      return {};
    }
    const auto value = entry.args[0];
    const auto fallible = types().entry(entry.args[1]).name != "never";
    if (!env_.needs_drop(value) &&
        !(fallible && env_.needs_drop(entry.args[1]))) {
      return {};
    }
    if (fallible) {
      return refuse(span, "the drop of a fallible task whose result needs a drop");
    }
    auto finished = classify_signature(env_.layouts,
                                       std::vector<type_id>{task.type},
                                       types().bool_type());
    if (!finished.has_value()) {
      return layout_failure(span, finished.error());
    }
    return branch_on(
        span,
        [&](const place &flag) {
          auto call = storage_op{.kind = op_kind::call_into};
          call.callee.name = "rt_task_is_finished";
          call.args.push_back(call_argument{.source_index = 0,
                                            .cls = finished->params[0].cls,
                                            .value = read_of(task, false)});
          call.signature = *finished;
          call.dest = flag;
          emit(std::move(call), span);
        },
        [&]() -> result {
          return branch_on(
              span,
              [&](const place &flag) {
                emit(storage_op{.kind = op_kind::coroutine_has_item,
                                .dest = flag,
                                .source = task},
                     span);
              },
              [&]() -> result {
                const auto item = new_slot(storage_class::frame_local, value, "",
                                           k_invalid_symbol_id, span, false);
                emit(storage_op{.kind = op_kind::generator_take,
                                .dest = whole(item),
                                .source = task},
                     span);
                if (auto ok = lower_value_drop(whole(item), span);
                    !ok.has_value()) {
                  return ok;
                }
                kill(item, span);
                return {};
              },
              [&]() -> result { return {}; });
        },
        [&]() -> result { return {}; });
  }

  /// The drops a value owes, in the order scheduled drops run them: its own
  /// `drop`, then each droppable field in declaration order.
  [[nodiscard]] auto lower_value_drop(const place &where,
                                      const source_span &span) -> result {
    const auto *plan = env_.drop_plan(where.type);
    if (plan == nullptr) {
      const auto layout = layout_of(where.type, span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      if ((*layout)->kind != runtime::value_layout_kind::tuple) {
        return refuse(span, "the drop of a `" + types().display(where.type) +
                                "` held by a task");
      }
      const auto &members = (*layout)->members;
      for (size_t i = 0; i < members.size(); ++i) {
        if (!env_.needs_drop(members[i].type)) {
          continue;
        }
        if (auto ok = lower_value_drop(
                project(where, member_step(i, ""), members[i].type), span);
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    }
    if (plan->cancels_generator || plan->closure_glue.has_value() ||
        plan->element.has_value()) {
      return refuse(span, "the drop of a `" + types().display(where.type) +
                              "` held by a task");
    }
    // The verifier learns a sum's variant from an `if` on a tag test of a whole
    // slot, so a sum that is part of a larger value is not dropped here.
    if (!plan->variant_drops.empty() && !where.steps.empty()) {
      return refuse(span, "the drop of a `" + types().display(where.type) +
                              "` inside another value");
    }
    if (plan->own_drop.has_value()) {
      const auto &resolved = *plan->own_drop;
      const auto receiver = env_.checked.types.ref_to(where.type, true);
      auto signature = classify_signature(env_.layouts,
                                          std::vector<type_id>{receiver},
                                          unit_type());
      if (!signature.has_value()) {
        return layout_failure(span, signature.error());
      }
      auto call = storage_op{.kind = op_kind::call_into};
      call.is_drop = true;
      call.callee.name = resolved.impl_target_type.empty()
                             ? resolved.decl->name
                             : resolved.impl_target_type +
                                   "::" + resolved.decl->name;
      call.callee.owner_module = resolved.owner_module;
      call.args.push_back(call_argument{
          .source_index = 0,
          .cls = signature->params[0].cls,
          .value = operand{.kind = operand_kind::address_of,
                           .where = where,
                           .type = receiver}});
      call.signature = std::move(*signature);
      const auto result_slot = new_slot(storage_class::frame_local, unit_type(),
                                        "", k_invalid_symbol_id, span, false);
      call.dest = whole(result_slot);
      emit(std::move(call), span);
      kill(result_slot, span);
    }
    if (!plan->droppable_fields.empty()) {
      const auto layout = layout_of(where.type, span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      for (const auto &[name, type] : plan->droppable_fields) {
        const auto &members = (*layout)->members;
        auto found = false;
        for (size_t i = 0; i < members.size() && !found; ++i) {
          if (members[i].name != name) {
            continue;
          }
          found = true;
          if (auto ok = lower_value_drop(
                  project(where, member_step(i, name), members[i].type), span);
              !ok.has_value()) {
            return ok;
          }
        }
        if (!found) {
          return malformed(span, "`" + types().display(where.type) +
                                     "` has no field `" + name + "`");
        }
      }
    }
    if (!plan->variant_drops.empty()) {
      return lower_variant_drops(where, *plan, 0, span);
    }
    return {};
  }

  /// Drops the payloads of the variant a sum holds: one tag test per variant
  /// that has droppable payloads, each directly in its `if`.
  [[nodiscard]] auto lower_variant_drops(const place &where,
                                         const semantic::drop_plan &plan,
                                         size_t index, const source_span &span)
      -> result {
    if (index == plan.variant_drops.size()) {
      return {};
    }
    const auto &vd = plan.variant_drops[index];
    const auto layout = layout_of(where.type, span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto *variant = (*layout)->find_variant(vd.variant);
    if (variant == nullptr) {
      return malformed(span, "`" + vd.variant + "` is not a variant of `" +
                                 types().display(where.type) + "`");
    }
    return branch_on(
        span,
        [&](const place &flag) {
          auto op = storage_op{.kind = op_kind::test_variant};
          op.dest = flag;
          op.source = where;
          op.variant = vd.variant;
          emit(std::move(op), span);
        },
        [&]() -> result {
          for (const auto &[slot, type] : vd.droppable_payloads) {
            if (slot >= variant->payload.size()) {
              return malformed(span, "`" + vd.variant + "` has no payload " +
                                         std::to_string(slot));
            }
            if (auto ok = lower_value_drop(
                    project(where,
                            projection{.kind = projection_kind::payload,
                                       .index = slot,
                                       .name = vd.variant},
                            variant->payload[slot].type),
                    span);
                !ok.has_value()) {
              return ok;
            }
          }
          return {};
        },
        [&]() -> result {
          return lower_variant_drops(where, plan, index + 1, span);
        });
  }

  /// `rt_task_block_on_all(group, n)` / `rt_task_block_on_any(group, n)`:
  /// outside a task, `await par` and `await race` wait with the thread. The
  /// group is a tuple of task handles, which the runtime reads in place.
  [[nodiscard]] auto lower_group_block(const hir_call &call, wait_mode mode,
                                       const place &dest) -> result {
    if (call.args.size() != 2 || !is_task_group_type(call.args[0]->type)) {
      return malformed(call.span, "a wait on a group of tasks needs a tuple of tasks");
    }
    auto group = lower_coroutine_place(*call.args[0]);
    if (!group.has_value()) {
      return std::unexpected(group.error());
    }
    auto block = storage_op{.kind = op_kind::task_block_on_group};
    block.waits = mode;
    block.source = std::move(*group);
    emit(std::move(block), call.span);
    if (dest.base != k_invalid_storage) {
      emit(storage_op{.kind = op_kind::init_empty, .dest = dest}, call.span);
    }
    return {};
  }

  /// Argument `i` of `call` as the storage form passes it. The legacy form
  /// hands a runtime intrinsic a heap value's own pointer where its
  /// signature says `&T`, so HIR passes `*x` there
  /// (`make_references_explicit`); in the storage form `T` is inline, and
  /// the intrinsic is given the reference `x` its signature names.
  [[nodiscard]] auto call_arg(const hir_call &call, size_t i) const
      -> const hir_expr & {
    const auto &arg = *call.args[i];
    if (call.target == nullptr || !call.target->modifiers.is_intrinsic ||
        i >= call.target->params.size() ||
        arg.kind != hir_node_kind::hir_unary ||
        as<hir_unary>(arg).op != ast::unary_op::deref) {
      return arg;
    }
    const auto &operand = *as<hir_unary>(arg).operand;
    const auto declared =
        env_.checked.node_types.find(call.target->params[i].pattern.get());
    if (declared == env_.checked.node_types.end() ||
        declared->second != operand.type) {
      return arg;
    }
    return operand;
  }

  [[nodiscard]] auto lower_call(const hir_call &call, const place &dest)
      -> result {
    if (call.callee->kind == hir_node_kind::hir_local_ref &&
        !locals_.contains(as<hir_local_ref>(*call.callee).symbol)) {
      const auto &name = as<hir_local_ref>(*call.callee).name;
      if (name == "rt_crew_task") {
        return refuse(call.span,
                      "`errors()` on a crew, which reads tasks the crew still "
                      "owns");
      }
      if (name == "rt_task_block_on_all" || name == "rt_task_block_on_any") {
        return lower_group_block(
            call, name == "rt_task_block_on_all" ? wait_mode::all : wait_mode::any,
            dest);
      }
    }
    // A callee that owns its receiver takes the value `&v` names, moved in.
    // A borrowed one (`receiver_borrowed`) is copied out of the reference.
    const hir_expr *owned_receiver = nullptr;
    auto borrowed_receiver = std::optional<type_id>{};
    if (call.receiver_by_value && !call.args.empty()) {
      const auto &first = *call.args.front();
      if (call.receiver_borrowed) {
        borrowed_receiver = env_.pointee(first.type);
        if (!borrowed_receiver.has_value()) {
          return malformed(call.span, "a borrowed receiver that is not a reference");
        }
      } else {
        // `&v`, or a block that binds a temporary and ends in `&tmp`, whose
        // statements run first.
        const auto *address = &first;
        if (first.kind == hir_node_kind::hir_block) {
          const auto &block = as<hir_block>(first);
          const auto *tail =
              block.stmts.empty() ||
                      block.stmts.back()->kind != hir_node_kind::hir_expr_stmt
                  ? nullptr
                  : as<hir_expr_stmt>(*block.stmts.back()).expr.get();
          if (tail != nullptr) {
            for (size_t i = 0; i + 1 < block.stmts.size(); ++i) {
              if (auto ok = lower_stmt(*block.stmts[i]); !ok.has_value()) {
                return ok;
              }
            }
            address = tail;
          }
        }
        if (address->kind != hir_node_kind::hir_unary ||
            (as<hir_unary>(*address).op != ast::unary_op::addr_of &&
             as<hir_unary>(*address).op != ast::unary_op::addr_of_mut)) {
          return refuse(call.span, "a consumed receiver that is not a place");
        }
        owned_receiver = as<hir_unary>(*address).operand.get();
      }
    }
    auto params = std::vector<type_id>{};
    for (size_t i = 0; i < call.args.size(); ++i) {
      params.push_back(i == 0 && owned_receiver != nullptr ? owned_receiver->type
                       : i == 0 && borrowed_receiver.has_value()
                           ? *borrowed_receiver
                           : call_arg(call, i).type);
    }
    auto signature = classify_signature(env_.layouts, params, call.type);
    if (!signature.has_value()) {
      return layout_failure(call.span, signature.error());
    }

    auto target = call_target{};
    // The runtime's task and crew calls look at a handle without taking it:
    // `t.spawn()` leaves `t` there to be awaited.
    auto borrows_handles = false;
    if (call.callee->kind == hir_node_kind::hir_local_ref &&
        !locals_.contains(as<hir_local_ref>(*call.callee).symbol)) {
      const auto &callee = as<hir_local_ref>(*call.callee);
      target.name = callee.name;
      target.owner_module = callee.owner_module;
      borrows_handles = target.name.starts_with("rt_task_") ||
                        target.name.starts_with("rt_crew_");
    } else {
      auto callable = lower_place(*call.callee);
      if (!callable.has_value()) {
        return std::unexpected(callable.error());
      }
      if (callable->has_value()) {
        target.callable = std::move(**callable);
      } else {
        const auto temp = new_temp(call.callee->type, call.callee->span);
        if (auto ok = lower_into(*call.callee, whole(temp)); !ok.has_value()) {
          return ok;
        }
        target.callable = whole(temp);
      }
    }

    auto op = storage_op{.kind = op_kind::call_into};
    op.is_drop = call.is_drop;
    // A candidate only: `confirm_tail_calls` keeps it where it is safe.
    op.tail_call = call.is_tail_call;
    auto outgoing = std::vector<storage_id>{};
    for (size_t i = 0; i < call.args.size(); ++i) {
      const auto &arg =
          i == 0 && owned_receiver != nullptr ? *owned_receiver : call_arg(call, i);
      const auto &param = signature->params[i];
      auto lowered = call_argument{.source_index = i, .cls = param.cls};
      if (i == 0 && borrowed_receiver.has_value()) {
        if (!is_indirect(param.cls)) {
          return refuse(call.span, "a borrowed receiver passed in a register");
        }
        const auto reference = new_temp(arg.type, arg.span);
        if (auto ok = lower_into(arg, whole(reference)); !ok.has_value()) {
          return ok;
        }
        const auto slot = new_slot(storage_class::outgoing_argument,
                                   *borrowed_receiver, "", k_invalid_symbol_id,
                                   arg.span, false);
        outgoing.push_back(slot);
        lowered.slot = whole(slot);
        auto copy = storage_op{.kind = op_kind::copy_into};
        copy.dest = lowered.slot;
        copy.source = project(whole(reference),
                              projection{.kind = projection_kind::deref},
                              *borrowed_receiver);
        copy.shallow = true;
        emit(std::move(copy), arg.span);
      } else if (is_indirect(param.cls) && !env_.is_copy(arg.type) &&
                 std::ranges::contains(call.lent_args, i)) {
        // A task block only borrows this capture: it gets a bitwise copy
        // that owns nothing, and the owner keeps the value.
        auto source = lower_place(arg);
        if (!source.has_value()) {
          return std::unexpected(source.error());
        }
        if (!source->has_value()) {
          return refuse(arg.span, "a borrowed task capture that is not a place");
        }
        const auto reference_type = env_.checked.types.ref_to(arg.type, false);
        const auto reference = new_temp(reference_type, arg.span);
        auto address = storage_op{.kind = op_kind::compute};
        address.dest = whole(reference);
        address.computation = compute_kind::copy;
        address.operands.push_back(operand{.kind = operand_kind::address_of,
                                           .where = std::move(**source),
                                           .type = reference_type});
        emit(std::move(address), arg.span);
        const auto slot = new_slot(storage_class::outgoing_argument, arg.type,
                                   "", k_invalid_symbol_id, arg.span, false);
        outgoing.push_back(slot);
        lowered.slot = whole(slot);
        auto copy = storage_op{.kind = op_kind::copy_into};
        copy.dest = lowered.slot;
        copy.source = project(whole(reference),
                              projection{.kind = projection_kind::deref},
                              arg.type);
        copy.shallow = true;
        emit(std::move(copy), arg.span);
      } else if (param.cls == pass_class::zero_sized) {
        const auto temp = new_temp(arg.type, arg.span);
        if (auto ok = lower_into(arg, whole(temp)); !ok.has_value()) {
          return ok;
        }
        consumed(temp);
      } else if (is_indirect(param.cls)) {
        const auto slot = new_slot(storage_class::outgoing_argument, arg.type,
                                   "", k_invalid_symbol_id, arg.span, false);
        outgoing.push_back(slot);
        lowered.slot = whole(slot);
        if (auto ok = lower_into(arg, lowered.slot); !ok.has_value()) {
          return ok;
        }
      } else {
        auto later_effects = false;
        for (size_t j = i + 1; j < call.args.size(); ++j) {
          later_effects = later_effects || !is_pure(*call.args[j]);
        }
        // A crew takes the task it is handed and gives it back as the
        // result, so only the crew itself is looked at.
        const auto hands_over = target.name == "rt_crew_spawn" && i == 1;
        auto value = lower_operand(arg, later_effects,
                                   borrows_handles && !hands_over);
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        lowered.value = std::move(*value);
      }
      op.args.push_back(std::move(lowered));
    }
    op.callee = std::move(target);
    op.signature = std::move(*signature);
    op.dest = dest;
    emit(std::move(op), call.span);
    if (borrowed_receiver.has_value()) {
      if (!dest.steps.empty() ||
          out_.slots[dest.base].cls != storage_class::frame_local) {
        return refuse(call.span, "an iterator over a borrowed collection that "
                                 "is not a local");
      }
      out_.slots[dest.base].owns_nothing = true;
    }
    for (const auto slot : outgoing) {
      auto dead = storage_op{.kind = op_kind::storage_dead};
      dead.slot = slot;
      emit(std::move(dead), call.span);
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Statements
  // ------------------------------------------------------------------

  [[nodiscard]] auto lower_stmt(const hir_node &node) -> result {
    switch (node.kind) {
    case hir_node_kind::hir_let:
      return lower_let(as<hir_let>(node));
    case hir_node_kind::hir_assign:
      return lower_assign(as<hir_assign>(node));
    case hir_node_kind::hir_expr_stmt:
      return lower_discarded(*as<hir_expr_stmt>(node).expr);
    case hir_node_kind::hir_return:
      return lower_return(as<hir_return>(node));
    case hir_node_kind::hir_yield:
      return lower_yield(as<hir_yield>(node));
    case hir_node_kind::hir_while:
      return lower_while(as<hir_while>(node));
    case hir_node_kind::hir_while_let:
      return lower_while_let(as<hir_while_let>(node));
    case hir_node_kind::hir_let_else:
      return lower_let_else(as<hir_let_else>(node));
    case hir_node_kind::hir_break:
      return lower_exit(node.span, loop_frame_kind::loop);
    case hir_node_kind::hir_continue:
      return lower_exit(node.span, loop_frame_kind::body);
    default:
      if (const auto *expr = dynamic_cast<const hir_expr *>(&node)) {
        return lower_discarded(*expr);
      }
      return refuse(node.span, describe(node.kind) +
                                   " is not part of storage lowering yet");
    }
  }

  [[nodiscard]] auto lower_let(const hir_let &let) -> result {
    if (let.name == "<let subject>" && let.initializer != nullptr) {
      // A destructuring `let` reads its parts from the subject, which HIR
      // never drops: a place is named in place, as a `match` subject is, so
      // binding `copy` parts of `*r` does not move out of the reference
      // (ch. 14, Matching).
      auto named = lower_place(*let.initializer);
      if (!named.has_value()) {
        return std::unexpected(named.error());
      }
      if (named->has_value()) {
        if ((*named)->steps.empty()) {
          locals_[let.symbol] = (*named)->base;
        } else {
          subjects_[let.symbol] = **named;
        }
        return flush_pending(let.span);
      }
    }
    const auto slot = new_slot(storage_class::frame_local, let.type(), let.name,
                               let.symbol, let.span, false);
    if (let.initializer != nullptr) {
      if (auto ok = lower_into(*let.initializer, whole(slot)); !ok.has_value()) {
        return ok;
      }
    }
    locals_[let.symbol] = slot;
    if (!scopes_.empty()) {
      scopes_.back().push_back(slot);
    }
    if (auto ended = flush_pending(let.span); !ended.has_value()) {
      return ended;
    }
    return {};
  }

  [[nodiscard]] auto lower_discarded(const hir_expr &expr) -> result {
    if (expr.kind == hir_node_kind::hir_block) {
      // A block's own type may be unsettled when nothing reads its value, so
      // its statements run in turn and only a trailing value gets a temporary.
      const auto &block = as<hir_block>(expr);
      for (size_t i = 0; i < block.stmts.size() && !terminated_; ++i) {
        const auto &stmt = *block.stmts[i];
        const auto *value = stmt.kind != hir_node_kind::hir_expr_stmt
                                ? dynamic_cast<const hir_expr *>(&stmt)
                                : nullptr;
        if (auto ok = value != nullptr ? lower_discarded(*value)
                                       : lower_stmt(stmt);
            !ok.has_value()) {
          return ok;
        }
      }
      return {};
    }
    if (expr.kind == hir_node_kind::hir_if) {
      return lower_if(as<hir_if>(expr), std::nullopt);
    }
    if (expr.kind == hir_node_kind::hir_match) {
      return lower_match(as<hir_match>(expr), std::nullopt);
    }
    if (expr.kind == hir_node_kind::hir_closure_drop) {
      // Its `unit` type is not settled in HIR, and nothing reads the value.
      return lower_closure_drop(as<hir_closure_drop>(expr), std::nullopt);
    }
    if (expr.kind == hir_node_kind::hir_coroutine_cancel) {
      return lower_coroutine_cancel(as<hir_coroutine_cancel>(expr), std::nullopt);
    }
    const auto temp = new_temp(expr.type, expr.span);
    if (auto ok = lower_into(expr, whole(temp)); !ok.has_value()) {
      return ok;
    }
    if (is_never(expr.type)) {
      // The call did not return, so nothing after it runs: not the end of
      // the statement's temporaries, and not the rest of the path.
      emit(storage_op{.kind = op_kind::unreachable}, expr.span);
      terminated_ = true;
      return {};
    }
    if (auto ended = flush_pending(expr.span); !ended.has_value()) {
      return ended;
    }
    return {};
  }

  // ------------------------------------------------------------------
  //  Control flow
  // ------------------------------------------------------------------

  /// Runs `body` with its own temporaries. Those of the enclosing expression
  /// stay alive across it and are set aside until it ends; its own end before
  /// the region does, unless the region has already left.
  template <typename F>
  [[nodiscard]] auto in_region(const source_span &span, F &&body) -> result {
    auto saved = std::move(pending_);
    pending_.clear();
    outer_pending_ += saved.size();
    auto ok = body();
    if (ok.has_value() && !terminated_) {
      ok = flush_pending(span);
    }
    pending_.clear();
    outer_pending_ -= saved.size();
    pending_ = std::move(saved);
    return ok;
  }

  auto kill(storage_id id, const source_span &span) -> void {
    auto dead = storage_op{.kind = op_kind::storage_dead};
    dead.slot = id;
    emit(std::move(dead), span);
  }

  /// A condition ready for `if_begin`: the operand it tests, and the slot
  /// holding it when that slot must end on each side of the branch. The
  /// statement's other temporaries have ended by the time it returns.
  struct condition {
    operand value;
    storage_id hold = k_invalid_storage;
  };

  [[nodiscard]] auto lower_condition(const hir_expr &expr)
      -> std::expected<condition, storage_lower_error> {
    auto value = lower_operand(expr, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto out = condition{.value = std::move(*value)};
    auto found = pending_.end();
    if (out.value.kind != operand_kind::constant) {
      found = std::ranges::find(pending_, out.value.where.base,
                                &pending_temp::slot);
    }
    if (found != pending_.end()) {
      if (out.value.kind == operand_kind::read && out.value.where.steps.empty()) {
        out.hold = found->slot;
        pending_.erase(found);
      } else {
        out.hold = new_slot(storage_class::frame_local, expr.type, "",
                            k_invalid_symbol_id, expr.span, false);
        auto copy = storage_op{.kind = op_kind::compute};
        copy.dest = whole(out.hold);
        copy.computation = compute_kind::copy;
        copy.operands.push_back(out.value);
        emit(std::move(copy), expr.span);
        out.value = read_of(whole(out.hold), false);
      }
    }
    if (auto ended = flush_pending(expr.span); !ended.has_value()) {
      return std::unexpected(ended.error());
    }
    return out;
  }

  /// `if`, with `elif` branches, as a statement (no `dest`) or as a value
  /// built in `dest`.
  [[nodiscard]] auto lower_if(const hir_if &node, const std::optional<place> &dest)
      -> result {
    if (dest.has_value() && node.else_body == nullptr) {
      return malformed(node.span, "an `if` that yields a value has no `else`");
    }
    return in_region(node.span,
                     [&] { return lower_branches(node, 0, dest); });
  }

  [[nodiscard]] auto lower_arm(const hir_block &body, const std::optional<place> &dest)
      -> result {
    return in_region(body.span, [&]() -> result {
      if (dest.has_value()) {
        return lower_block(body, *dest, true);
      }
      return lower_discarded(body);
    });
  }

  [[nodiscard]] auto lower_branches(const hir_if &node, size_t index,
                                    const std::optional<place> &dest) -> result {
    const auto &branch = node.branches[index];
    auto test = lower_condition(*branch.condition);
    if (!test.has_value()) {
      return std::unexpected(test.error());
    }
    const auto has_else =
        index + 1 < node.branches.size() || node.else_body != nullptr;
    auto head = storage_op{.kind = op_kind::if_begin};
    head.condition = test->value;
    emit(std::move(head), branch.condition->span);

    if (test->hold != k_invalid_storage) {
      kill(test->hold, branch.condition->span);
    }
    if (auto ok = lower_arm(*branch.body, dest); !ok.has_value()) {
      return ok;
    }
    const auto then_terminated = terminated_;
    terminated_ = false;
    if (has_else || test->hold != k_invalid_storage) {
      emit(storage_op{.kind = op_kind::else_begin}, node.span);
      if (test->hold != k_invalid_storage) {
        kill(test->hold, branch.condition->span);
      }
      if (index + 1 < node.branches.size()) {
        auto ok = in_region(node.span,
                            [&] { return lower_branches(node, index + 1, dest); });
        if (!ok.has_value()) {
          return ok;
        }
      } else if (node.else_body != nullptr) {
        if (auto ok = lower_arm(*node.else_body, dest); !ok.has_value()) {
          return ok;
        }
      }
    }
    emit(storage_op{.kind = op_kind::if_end}, node.span);
    terminated_ = then_terminated && terminated_;
    return {};
  }

  /// `while`: a loop that tests its condition and leaves when it fails, runs
  /// the body in a block that `continue` leaves, then the step, then repeats.
  [[nodiscard]] auto lower_while(const hir_while &loop) -> result {
    const auto always =
        loop.condition->kind == hir_node_kind::hir_literal &&
        as<hir_literal>(*loop.condition).lit_kind == token_kind::kw_true;
    emit(storage_op{.kind = op_kind::loop_begin}, loop.span);
    frames_.push_back(loop_frame{.kind = loop_frame_kind::loop, .partials = partials_.size()});
    scopes_.emplace_back();
    if (!always) {
      auto ok = in_region(loop.condition->span, [&]() -> result {
        auto test = lower_condition(*loop.condition);
        if (!test.has_value()) {
          return std::unexpected(test.error());
        }
        auto head = storage_op{.kind = op_kind::if_begin};
        head.condition = test->value;
        emit(std::move(head), loop.condition->span);
        if (test->hold != k_invalid_storage) {
          kill(test->hold, loop.condition->span);
        }
        emit(storage_op{.kind = op_kind::else_begin}, loop.span);
        if (test->hold != k_invalid_storage) {
          kill(test->hold, loop.condition->span);
        }
        auto leave = storage_op{.kind = op_kind::exit};
        leave.depth = 0;
        emit(std::move(leave), loop.span);
        emit(storage_op{.kind = op_kind::if_end}, loop.span);
        return {};
      });
      if (!ok.has_value()) {
        return ok;
      }
      frames_.back().reached = true;
    }

    return finish_loop(*loop.body, loop.step.get(), loop.span);
  }

  /// The body of a loop whose head has been emitted: the block that
  /// `continue` leaves, the step, the end of the body's storage, and the
  /// back edge.
  [[nodiscard]] auto finish_loop(const hir_block &body, const hir_block *step,
                                 const source_span &span) -> result {
    const auto head_storage = scopes_.back().size();
    emit(storage_op{.kind = op_kind::block_begin}, body.span);
    frames_.push_back(loop_frame{.kind = loop_frame_kind::body, .partials = partials_.size()});
    auto ok = in_region(body.span, [&] { return lower_discarded(body); });
    if (!ok.has_value()) {
      return ok;
    }
    const auto continued = frames_.back().reached;
    frames_.pop_back();
    emit(storage_op{.kind = op_kind::block_end}, body.span);
    terminated_ = terminated_ && !continued;

    if (!terminated_ && step != nullptr) {
      auto stepped =
          in_region(step->span, [&] { return lower_discarded(*step); });
      if (!stepped.has_value()) {
        return stepped;
      }
    }
    if (!terminated_) {
      // Storage declared in the body starts over on the next iteration.
      for (auto it = scopes_.back().rbegin(); it != scopes_.back().rend(); ++it) {
        kill(*it, span);
      }
    }
    emit(storage_op{.kind = op_kind::loop_end}, span);
    const auto left = frames_.back().reached;
    frames_.pop_back();
    if (left) {
      // Every path out of the loop leaves the head's storage (a `while let`
      // subject) live; it ends here, so an enclosing loop body ends as it
      // began.
      for (auto i = head_storage; i > 0; --i) {
        kill(scopes_.back()[i - 1], span);
      }
    }
    scopes_.pop_back();
    terminated_ = !left;
    return {};
  }

  // ------------------------------------------------------------------
  //  Patterns: match, while let, let else
  // ------------------------------------------------------------------

  /// A part of the subject and the pattern it must satisfy.
  struct obligation {
    const hir_pattern *pattern = nullptr;
    place where;
  };
  using continuation = std::function<result()>;

  /// The subject of a pattern test. A subject that is a local is used where
  /// it stands, and a computed one is built in `held`, which the caller ends.
  struct subject_place {
    place where;
    storage_id held = k_invalid_storage;
  };

  /// Whether a sum that owes drops can be tested with `pattern`: variant tests
  /// (nested ones too), literals on scalars, and the structure around them. The
  /// verifier learns which variant a sum holds only from an `if` directly on
  /// the tag test, so every alternative of an or-pattern would have to agree on
  /// it; those are refused.
  [[nodiscard]] auto tests_supported_on_droppable(
      const hir_pattern &pattern) const -> bool {
    switch (pattern.kind) {
    case hir_node_kind::hir_wildcard_pattern:
    case hir_node_kind::hir_literal_pattern:
      return true;
    case hir_node_kind::hir_constructor_pattern:
      return std::ranges::all_of(
          as<hir_constructor_pattern>(pattern).args,
          [&](const auto &arg) { return tests_supported_on_droppable(*arg); });
    case hir_node_kind::hir_tuple_pattern:
      return std::ranges::all_of(
          as<hir_tuple_pattern>(pattern).elements,
          [&](const auto &arg) { return tests_supported_on_droppable(*arg); });
    case hir_node_kind::hir_array_pattern:
      return std::ranges::all_of(
          as<hir_array_pattern>(pattern).elements,
          [&](const auto &arg) { return tests_supported_on_droppable(*arg); });
    case hir_node_kind::hir_struct_pattern:
      return std::ranges::all_of(
          as<hir_struct_pattern>(pattern).fields, [&](const auto &field) {
            return tests_supported_on_droppable(*field.pattern);
          });
    default:
      return false;
    }
  }

  /// A sum that needs a drop can be tested by its variants. Its payloads are
  /// moved or dropped inside the arm that tested for them.
  [[nodiscard]] auto check_droppable_subject(const hir_expr &subject,
                                             const hir_pattern &pattern,
                                             const source_span &span)
      -> result {
    if (!env_.needs_drop(subject.type)) {
      return {};
    }
    const auto layout = layout_of(subject.type, span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    if ((*layout)->kind != runtime::value_layout_kind::sum) {
      return refuse(span, "a pattern test on `" + types().display(subject.type) +
                              "`, which needs a drop");
    }
    if (!tests_supported_on_droppable(pattern)) {
      return refuse(span, "a pattern on `" + types().display(subject.type) +
                              "`, which needs a drop, that uses an "
                              "or-pattern or a range");
    }
    return {};
  }

  [[nodiscard]] auto bind_subject(const hir_expr &subject, symbol_id symbol,
                                  const source_span &span)
      -> std::expected<subject_place, storage_lower_error> {
    auto named = lower_place(subject);
    if (!named.has_value()) {
      return std::unexpected(named.error());
    }
    if (named->has_value()) {
      if (!(*named)->steps.empty()) {
        // The subject is a part of a value, as in the match HIR writes to drop
        // a sum nested in another: the symbol names that part.
        subjects_[symbol] = **named;
        return subject_place{.where = std::move(**named)};
      }
      locals_[symbol] = (*named)->base;
      return subject_place{.where = std::move(**named)};
    }
    const auto slot = new_slot(storage_class::frame_local, subject.type, "",
                               k_invalid_symbol_id, span, false);
    auto built = in_region(span, [&] { return lower_into(subject, whole(slot)); });
    if (!built.has_value()) {
      return std::unexpected(built.error());
    }
    locals_[symbol] = slot;
    return subject_place{.where = whole(slot), .held = slot};
  }

  auto set_bool(const place &dest, bool value, const source_span &span) -> void {
    auto op = storage_op{.kind = op_kind::compute};
    op.dest = dest;
    op.computation = compute_kind::constant;
    op.literal = value ? token_kind::kw_true : token_kind::kw_false;
    op.text = value ? "true" : "false";
    emit(std::move(op), span);
  }

  /// Emits `test` into a flag, then `then_side` where it holds and `else_side`
  /// where it does not. The flag ends on both sides.
  template <typename Test>
  [[nodiscard]] auto branch_on(const source_span &span, Test &&test,
                               const continuation &then_side,
                               const continuation &else_side) -> result {
    const auto flag = new_slot(storage_class::frame_local, types().bool_type(),
                               "", k_invalid_symbol_id, span, false);
    test(whole(flag));
    auto head = storage_op{.kind = op_kind::if_begin};
    head.condition = read_of(whole(flag), false);
    emit(std::move(head), span);
    kill(flag, span);
    if (auto ok = then_side(); !ok.has_value()) {
      return ok;
    }
    emit(storage_op{.kind = op_kind::else_begin}, span);
    kill(flag, span);
    if (auto ok = else_side(); !ok.has_value()) {
      return ok;
    }
    emit(storage_op{.kind = op_kind::if_end}, span);
    return {};
  }

  /// Emits the tests of `todo` in order, each nested in the one before so a
  /// payload is read only where its variant was confirmed. `success` runs where
  /// all of them hold and `failure` where one does not.
  [[nodiscard]] auto emit_tests(std::vector<obligation> todo,
                                const continuation &success,
                                const continuation &failure) -> result {
    if (todo.empty()) {
      return success();
    }
    const auto next = todo.front();
    todo.erase(todo.begin());
    const auto &pattern = *next.pattern;
    const auto span = pattern.span;
    switch (pattern.kind) {
    case hir_node_kind::hir_wildcard_pattern:
      return emit_tests(std::move(todo), success, failure);
    case hir_node_kind::hir_constructor_pattern: {
      const auto &ctor = as<hir_constructor_pattern>(pattern);
      auto object = auto_deref(next.where);
      const auto layout = layout_of(object.type, span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto *variant = (*layout)->find_variant(ctor.variant_name);
      if (variant == nullptr ||
          (*layout)->kind != runtime::value_layout_kind::sum ||
          variant->payload.size() != ctor.args.size()) {
        return malformed(span, "`@" + ctor.variant_name +
                                   "` is not a variant of `" +
                                   types().display(object.type) + "`");
      }
      auto inner = std::vector<obligation>{};
      for (size_t i = 0; i < ctor.args.size(); ++i) {
        inner.push_back(obligation{
            .pattern = ctor.args[i].get(),
            .where = project(object,
                             projection{.kind = projection_kind::payload,
                                        .index = i,
                                        .name = ctor.variant_name},
                             variant->payload[i].type)});
      }
      inner.insert(inner.end(), todo.begin(), todo.end());
      return branch_on(
          span,
          [&](const place &flag) {
            auto op = storage_op{.kind = op_kind::test_variant};
            op.dest = flag;
            op.source = object;
            op.variant = ctor.variant_name;
            emit(std::move(op), span);
          },
          [&] { return emit_tests(inner, success, failure); }, failure);
    }
    case hir_node_kind::hir_literal_pattern: {
      const auto &literal = as<hir_literal_pattern>(pattern);
      const auto cls = class_of(next.where.type, span);
      if (!cls.has_value()) {
        return std::unexpected(cls.error());
      }
      if (*cls != pass_class::scalar ||
          literal.lit_kind == token_kind::string_lit) {
        return refuse(span, "a literal pattern on `" +
                                types().display(next.where.type) + "`");
      }
      return branch_on(
          span,
          [&](const place &flag) {
            auto op = storage_op{.kind = op_kind::compute};
            op.dest = flag;
            op.computation = compute_kind::binary;
            op.binary = ast::binary_op::eq_eq;
            op.operands.push_back(read_of(next.where, false));
            op.operands.push_back(operand{.kind = operand_kind::constant,
                                          .type = next.where.type,
                                          .literal = literal.lit_kind,
                                          .text = literal.value});
            emit(std::move(op), span);
          },
          [&] { return emit_tests(todo, success, failure); }, failure);
    }
    case hir_node_kind::hir_tuple_pattern:
    case hir_node_kind::hir_array_pattern: {
      const auto &elements =
          pattern.kind == hir_node_kind::hir_tuple_pattern
              ? as<hir_tuple_pattern>(pattern).elements
              : as<hir_array_pattern>(pattern).elements;
      auto object = auto_deref(next.where);
      const auto layout = layout_of(object.type, span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &found = **layout;
      auto inner = std::vector<obligation>{};
      if (found.kind == runtime::value_layout_kind::array) {
        // A fixed array is matched element by element, by a tuple pattern
        // or an array pattern of its length.
        if (found.count != elements.size()) {
          return malformed(span, "a pattern of " + std::to_string(elements.size()) +
                                     " elements does not match `" +
                                     types().display(object.type) + "`");
        }
        for (size_t i = 0; i < elements.size(); ++i) {
          inner.push_back(obligation{
              .pattern = elements[i].get(),
              .where = project(object,
                               projection{.kind = projection_kind::element,
                                          .index = i},
                               found.element)});
        }
      } else if (pattern.kind == hir_node_kind::hir_array_pattern) {
        return refuse(span, "an array pattern on `" + types().display(object.type) +
                                "`, whose length is not part of its type");
      } else {
        const auto &members = found.members;
        if (members.size() != elements.size()) {
          return malformed(span, "a tuple pattern does not match `" +
                                     types().display(object.type) + "`");
        }
        for (size_t i = 0; i < members.size(); ++i) {
          inner.push_back(obligation{
              .pattern = elements[i].get(),
              .where = project(object, member_step(i, ""), members[i].type)});
        }
      }
      inner.insert(inner.end(), todo.begin(), todo.end());
      return emit_tests(std::move(inner), success, failure);
    }
    case hir_node_kind::hir_struct_pattern: {
      const auto &record = as<hir_struct_pattern>(pattern);
      auto object = auto_deref(next.where);
      const auto layout = layout_of(object.type, span);
      if (!layout.has_value()) {
        return std::unexpected(layout.error());
      }
      const auto &members = (*layout)->members;
      auto inner = std::vector<obligation>{};
      for (const auto &field : record.fields) {
        auto found = false;
        for (size_t i = 0; i < members.size() && !found; ++i) {
          if (members[i].name != field.name) {
            continue;
          }
          found = true;
          inner.push_back(obligation{
              .pattern = field.pattern.get(),
              .where = project(object, member_step(i, field.name),
                               members[i].type)});
        }
        if (!found) {
          return malformed(span, "`" + types().display(object.type) +
                                     "` has no field `" + field.name + "`");
        }
      }
      inner.insert(inner.end(), todo.begin(), todo.end());
      return emit_tests(std::move(inner), success, failure);
    }
    case hir_node_kind::hir_or_pattern:
      return emit_alternatives(as<hir_or_pattern>(pattern), 0, next.where, todo,
                               success, failure);
    default:
      return refuse(span, "this kind of pattern is not part of storage "
                          "lowering yet");
    }
  }

  /// `a | b | c`: each alternative is tried in turn against the same part of
  /// the subject, and the first that holds continues with the rest.
  [[nodiscard]] auto emit_alternatives(const hir_or_pattern &any, size_t index,
                                       const place &where,
                                       const std::vector<obligation> &rest,
                                       const continuation &success,
                                       const continuation &failure) -> result {
    if (index == any.alternatives.size()) {
      return failure();
    }
    auto todo = std::vector<obligation>{
        obligation{.pattern = any.alternatives[index].get(), .where = where}};
    todo.insert(todo.end(), rest.begin(), rest.end());
    return emit_tests(
        std::move(todo), success, [&, index] {
          return emit_alternatives(any, index + 1, where, rest, success,
                                   failure);
        });
  }

  /// Whether the pattern holds for every value of its type.
  [[nodiscard]] auto always_matches(const hir_pattern &pattern) const -> bool {
    switch (pattern.kind) {
    case hir_node_kind::hir_wildcard_pattern:
      return true;
    case hir_node_kind::hir_tuple_pattern:
      return std::ranges::all_of(
          as<hir_tuple_pattern>(pattern).elements,
          [&](const auto &element) { return always_matches(*element); });
    case hir_node_kind::hir_struct_pattern:
      return std::ranges::all_of(
          as<hir_struct_pattern>(pattern).fields,
          [&](const auto &field) { return always_matches(*field.pattern); });
    default:
      return false;
    }
  }

  /// Sets `flag` to whether `pattern` holds for `where`.
  [[nodiscard]] auto test_pattern(const hir_pattern &pattern,
                                  const place &where, const place &flag)
      -> result {
    const auto span = pattern.span;
    return emit_tests(
        {obligation{.pattern = &pattern, .where = where}},
        [&] {
          set_bool(flag, true, span);
          return result{};
        },
        [&] {
          set_bool(flag, false, span);
          return result{};
        });
  }

  /// `match`: the arms are tried in order, and the first whose pattern holds
  /// runs. An arm that holds for every value, and the last arm, need no test.
  [[nodiscard]] auto lower_match(const hir_match &node,
                                 const std::optional<place> &dest) -> result {
    for (const auto &arm : node.arms) {
      if (auto ok = check_droppable_subject(*node.subject, *arm.pattern,
                                            arm.pattern->span);
          !ok.has_value()) {
        return ok;
      }
    }
    auto subject = bind_subject(*node.subject, node.subject_symbol, node.span);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    if (subject->held != k_invalid_storage) {
      open_subjects_.push_back(
          open_subject{.slot = subject->held, .frames = frames_.size()});
    }
    auto ok = in_region(node.span, [&] {
      return lower_arms(node, 0, subject->where, dest);
    });
    if (subject->held != k_invalid_storage) {
      open_subjects_.pop_back();
    }
    if (!ok.has_value()) {
      return ok;
    }
    if (subject->held != k_invalid_storage && !terminated_) {
      kill(subject->held, node.span);
    }
    return {};
  }

  [[nodiscard]] auto lower_arms(const hir_match &node, size_t index,
                                const place &subject,
                                const std::optional<place> &dest) -> result {
    if (index == node.arms.size()) {
      return {};
    }
    const auto &arm = node.arms[index];
    const auto last = index + 1 == node.arms.size();
    if (arm.guard != nullptr) {
      return lower_guarded_arm(node, index, subject, dest);
    }
    // The checker requires a `match` to cover every value, so the last arm
    // holds wherever the earlier ones did not.
    if (always_matches(*arm.pattern) || last) {
      return lower_arm(*arm.body, dest);
    }
    if (env_.needs_drop(subject.type)) {
      // Test the tag directly in the `if`, so each side knows the variant.
      auto arm_terminated = false;
      auto ok = emit_tests(
          {obligation{.pattern = arm.pattern.get(), .where = subject}},
          [&]() -> result {
            auto ran = lower_arm(*arm.body, dest);
            arm_terminated = terminated_;
            terminated_ = false;
            return ran;
          },
          [&]() -> result {
            // Each failure site holds a copy of the remaining arms.
            terminated_ = false;
            return in_region(node.span, [&] {
              return lower_arms(node, index + 1, subject, dest);
            });
          });
      if (!ok.has_value()) {
        return ok;
      }
      terminated_ = arm_terminated && terminated_;
      return {};
    }
    const auto flag = new_slot(storage_class::frame_local, types().bool_type(),
                               "", k_invalid_symbol_id, arm.pattern->span, false);
    if (auto ok = test_pattern(*arm.pattern, subject, whole(flag));
        !ok.has_value()) {
      return ok;
    }
    auto head = storage_op{.kind = op_kind::if_begin};
    head.condition = read_of(whole(flag), false);
    emit(std::move(head), arm.pattern->span);
    kill(flag, arm.pattern->span);
    if (auto ok = lower_arm(*arm.body, dest); !ok.has_value()) {
      return ok;
    }
    const auto arm_terminated = terminated_;
    terminated_ = false;
    emit(storage_op{.kind = op_kind::else_begin}, node.span);
    kill(flag, arm.pattern->span);
    if (!last) {
      auto ok = in_region(node.span, [&] {
        return lower_arms(node, index + 1, subject, dest);
      });
      if (!ok.has_value()) {
        return ok;
      }
    }
    emit(storage_op{.kind = op_kind::if_end}, node.span);
    terminated_ = arm_terminated && terminated_;
    return {};
  }

  /// An arm with a guard. The pattern is tested, then the bindings HIR placed
  /// ahead of the guard run and the guard is evaluated; the body runs when it
  /// holds. A pattern that does not hold, and a guard that does not, both go on
  /// to the remaining arms, so those are built at each of the two places.
  ///
  /// A failed guard leaves the subject as it was, so the bindings must not take
  /// anything from it that needs a drop.
  [[nodiscard]] auto lower_guarded_arm(const hir_match &node, size_t index,
                                       const place &subject,
                                       const std::optional<place> &dest)
      -> result {
    const auto &arm = node.arms[index];
    const auto last = index + 1 == node.arms.size();
    const auto *condition_expr = arm.guard.get();
    const hir_block *bindings = nullptr;
    if (arm.guard->kind == hir_node_kind::hir_block) {
      bindings = &as<hir_block>(*arm.guard);
      const auto &stmts = bindings->stmts;
      const auto *tail = stmts.empty() ? nullptr : stmts.back().get();
      if (tail == nullptr || tail->kind != hir_node_kind::hir_expr_stmt) {
        return malformed(arm.guard->span, "a `match` arm guard with no value");
      }
      condition_expr = as<hir_expr_stmt>(*tail).expr.get();
      for (size_t i = 0; i + 1 < stmts.size(); ++i) {
        if (stmts[i]->kind != hir_node_kind::hir_let) {
          // Something HIR runs before the guard, such as the drop of a payload
          // the pattern does not name; a failed guard would find it done.
          return refuse(stmts[i]->span,
                        "a `match` arm guard on a pattern that drops part of "
                        "the value before the guard is tested");
        }
        if (env_.needs_drop(as<hir_let>(*stmts[i]).type())) {
          return refuse(stmts[i]->span,
                        "a `match` arm guard on a pattern that binds `" +
                            as<hir_let>(*stmts[i]).name +
                            "`, a value that needs a drop");
        }
      }
    }
    auto arm_terminated = false;
    auto succeed = [&]() -> result {
      return in_region(arm.guard->span, [&]() -> result {
        if (bindings != nullptr) {
          for (size_t i = 0; i + 1 < bindings->stmts.size() && !terminated_;
               ++i) {
            if (auto ok = lower_stmt(*bindings->stmts[i]); !ok.has_value()) {
              return ok;
            }
          }
        }
        if (last) {
          // Nothing follows the last arm: the checker has shown the earlier
          // ones leave no value for it to refuse.
          if (auto ok = lower_discarded(*condition_expr); !ok.has_value()) {
            return ok;
          }
          auto ran = lower_arm(*arm.body, dest);
          arm_terminated = terminated_;
          return ran;
        }
        auto test = lower_condition(*condition_expr);
        if (!test.has_value()) {
          return std::unexpected(test.error());
        }
        auto head = storage_op{.kind = op_kind::if_begin};
        head.condition = test->value;
        emit(std::move(head), arm.guard->span);
        if (test->hold != k_invalid_storage) {
          kill(test->hold, arm.guard->span);
        }
        if (auto ok = lower_arm(*arm.body, dest); !ok.has_value()) {
          return ok;
        }
        const auto body_terminated = terminated_;
        terminated_ = false;
        emit(storage_op{.kind = op_kind::else_begin}, arm.guard->span);
        if (test->hold != k_invalid_storage) {
          kill(test->hold, arm.guard->span);
        }
        auto rest = in_region(node.span, [&] {
          return lower_arms(node, index + 1, subject, dest);
        });
        if (!rest.has_value()) {
          return rest;
        }
        emit(storage_op{.kind = op_kind::if_end}, arm.guard->span);
        arm_terminated = body_terminated && terminated_;
        return result{};
      });
    };
    if (always_matches(*arm.pattern) || last) {
      auto ok = succeed();
      terminated_ = arm_terminated;
      return ok;
    }
    auto ok = emit_tests(
        {obligation{.pattern = arm.pattern.get(), .where = subject}},
        [&]() -> result {
          auto ran = succeed();
          terminated_ = false;
          return ran;
        },
        [&]() -> result {
          terminated_ = false;
          return in_region(node.span, [&] {
            return lower_arms(node, index + 1, subject, dest);
          });
        });
    if (!ok.has_value()) {
      return ok;
    }
    terminated_ = arm_terminated && terminated_;
    return {};
  }

  /// `while let`: a loop that evaluates the subject, leaves when the pattern
  /// does not hold, and otherwise runs the body.
  [[nodiscard]] auto lower_while_let(const hir_while_let &loop) -> result {
    emit(storage_op{.kind = op_kind::loop_begin}, loop.span);
    frames_.push_back(loop_frame{.kind = loop_frame_kind::loop, .partials = partials_.size()});
    scopes_.emplace_back();
    auto head = in_region(loop.span, [&]() -> result {
      // A `for` over values that need a drop arrives here as a `while let`
      // over the iterator's `option`. Its variants are tested like a `let
      // else`: the payload is moved or dropped in the body, and the loop is
      // left where the pattern fails.
      if (auto ok = check_droppable_subject(*loop.subject, *loop.pattern,
                                            loop.span);
          !ok.has_value()) {
        return ok;
      }
      auto subject = bind_subject(*loop.subject, loop.subject_symbol, loop.span);
      if (!subject.has_value()) {
        return std::unexpected(subject.error());
      }
      if (subject->held != k_invalid_storage) {
        scopes_.back().push_back(subject->held);
      }
      if (env_.needs_drop(subject->where.type)) {
        auto ok = emit_tests(
            {obligation{.pattern = loop.pattern.get(), .where = subject->where}},
            []() -> result { return {}; }, [&]() -> result {
              auto leave = storage_op{.kind = op_kind::exit};
              leave.depth = 0;
              emit(std::move(leave), loop.span);
              return {};
            });
        terminated_ = false;
        return ok;
      }
      const auto flag = new_slot(storage_class::frame_local,
                                 types().bool_type(), "", k_invalid_symbol_id,
                                 loop.span, false);
      if (auto ok = test_pattern(*loop.pattern, subject->where, whole(flag));
          !ok.has_value()) {
        return ok;
      }
      auto test = storage_op{.kind = op_kind::if_begin};
      test.condition = read_of(whole(flag), false);
      emit(std::move(test), loop.span);
      kill(flag, loop.span);
      emit(storage_op{.kind = op_kind::else_begin}, loop.span);
      kill(flag, loop.span);
      // The subject stays as it is, like any other storage the body declares:
      // a `break` leaves it live, and the paths that leave the loop must agree.
      auto leave = storage_op{.kind = op_kind::exit};
      leave.depth = 0;
      emit(std::move(leave), loop.span);
      emit(storage_op{.kind = op_kind::if_end}, loop.span);
      return {};
    });
    if (!head.has_value()) {
      return head;
    }
    frames_.back().reached = true;
    return finish_loop(*loop.body, nullptr, loop.span);
  }

  /// `let pattern = value else { ... }`: the else block leaves, so the code
  /// after this statement runs only where the pattern held.
  [[nodiscard]] auto lower_let_else(const hir_let_else &node) -> result {
    if (auto ok = check_droppable_subject(*node.initializer, *node.pattern,
                                          node.span);
        !ok.has_value()) {
      return ok;
    }
    auto subject =
        bind_subject(*node.initializer, node.subject_symbol, node.span);
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    if (subject->held != k_invalid_storage && !scopes_.empty()) {
      scopes_.back().push_back(subject->held);
    }
    if (env_.needs_drop(subject->where.type)) {
      auto ok = emit_tests(
          {obligation{.pattern = node.pattern.get(), .where = subject->where}},
          []() -> result { return {}; }, [&]() -> result {
            // A nested pattern has several failure sites, each a copy of the
            // `else` block; one that leaves must not hide the next.
            terminated_ = false;
            return lower_arm(*node.else_body, std::nullopt);
          });
      terminated_ = false;
      return ok;
    }
    const auto flag = new_slot(storage_class::frame_local, types().bool_type(),
                               "", k_invalid_symbol_id, node.span, false);
    if (auto ok = test_pattern(*node.pattern, subject->where, whole(flag));
        !ok.has_value()) {
      return ok;
    }
    auto head = storage_op{.kind = op_kind::if_begin};
    head.condition = read_of(whole(flag), false);
    emit(std::move(head), node.span);
    kill(flag, node.span);
    emit(storage_op{.kind = op_kind::else_begin}, node.span);
    kill(flag, node.span);
    if (auto ok = lower_arm(*node.else_body, std::nullopt); !ok.has_value()) {
      return ok;
    }
    emit(storage_op{.kind = op_kind::if_end}, node.span);
    terminated_ = false;
    return {};
  }

  /// `break` leaves the innermost loop; `continue` leaves its body block.
  [[nodiscard]] auto lower_exit(const source_span &span, loop_frame_kind target)
      -> result {
    if (outer_pending_ != 0) {
      return refuse(span, "`break` or `continue` inside an expression that has "
                          "live temporaries");
    }
    for (size_t depth = 0; depth < frames_.size(); ++depth) {
      auto &frame = frames_[frames_.size() - 1 - depth];
      if (frame.kind != target) {
        continue;
      }
      if (partials_.size() > frame.partials) {
        return refuse(span, "`break` or `continue` inside a value under "
                            "construction that holds values needing a drop");
      }
      frame.reached = true;
      // A computed `match` subject inside the region being left ends here
      // (`x or else continue`), as it does where the match ends.
      const auto target_index = frames_.size() - 1 - depth;
      for (auto it = open_subjects_.rbegin(); it != open_subjects_.rend();
           ++it) {
        if (it->frames > target_index &&
            !env_.needs_drop(out_.slots[it->slot].type)) {
          kill(it->slot, span);
        }
      }
      auto leave = storage_op{.kind = op_kind::exit};
      leave.depth = depth;
      emit(std::move(leave), span);
      terminated_ = true;
      return {};
    }
    return malformed(span, "`break` or `continue` outside a loop");
  }

  [[nodiscard]] auto lower_assign(const hir_assign &assign) -> result {
    if (assign.op != ast::assign_op::assign) {
      return lower_compound_assign(assign);
    }
    auto target = lower_place(*assign.target);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    if (!target->has_value()) {
      return malformed(assign.span, "the target of an assignment is not a place");
    }
    const auto &where = **target;
    const auto cls = class_of(where.type, assign.span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    if (*cls == pass_class::scalar || *cls == pass_class::address) {
      // One operation reads its operands before it writes, so the target can
      // be written directly.
      if (auto ok = lower_into(*assign.value, where); !ok.has_value()) {
        return ok;
      }
    } else if (env_.needs_drop(where.type)) {
      // HIR has already staged the replacement: the new value is built in a
      // local, then the old value's drop calls run, then this assignment
      // stores the local. The target holds no live value here, so the
      // replacement is written straight in; the verifier rejects a write
      // over a value whose drops have not run.
      if (auto ok = lower_into(*assign.value, where); !ok.has_value()) {
        return ok;
      }
    } else {
      // Build the replacement apart from the target, because building it may
      // read the target. Then move it in.
      const auto replacement = new_temp(where.type, assign.span);
      if (auto ok = lower_into(*assign.value, whole(replacement));
          !ok.has_value()) {
        return ok;
      }
      if (*cls != pass_class::zero_sized) {
        auto move = storage_op{.kind = op_kind::move_into};
        move.dest = where;
        move.source = whole(replacement);
        emit(std::move(move), assign.span);
      }
      consumed(replacement);
    }
    if (auto ended = flush_pending(assign.span); !ended.has_value()) {
      return ended;
    }
    return {};
  }

  /// `c.set(v)` on a `cell_mut[T]`: an assignment through the address `c`
  /// is. HIR stages no drop of the old value here, so an element that needs
  /// one is refused.
  [[nodiscard]] auto lower_cell_set(const hir_cell_set &set,
                                    const std::optional<place> &dest) -> result {
    auto cell = lower_place(*set.cell);
    if (!cell.has_value()) {
      return std::unexpected(cell.error());
    }
    auto address = place{};
    if (cell->has_value()) {
      address = std::move(**cell);
    } else {
      const auto temp = new_temp(set.cell->type, set.cell->span);
      if (auto ok = lower_into(*set.cell, whole(temp)); !ok.has_value()) {
        return ok;
      }
      address = whole(temp);
    }
    const auto element = env_.pointee(set.cell->type);
    if (!element.has_value()) {
      return malformed(set.span, "`set` on a value that is not a cell");
    }
    if (env_.needs_drop(*element)) {
      return refuse(set.span, "`set` on a cell of " + types().display(*element) +
                                  ", whose old value needs a drop");
    }
    const auto target = project(std::move(address),
                                projection{.kind = projection_kind::deref},
                                *element);
    const auto cls = class_of(*element, set.span);
    if (!cls.has_value()) {
      return std::unexpected(cls.error());
    }
    if (*cls == pass_class::scalar || *cls == pass_class::address) {
      if (auto ok = lower_into(*set.value, target); !ok.has_value()) {
        return ok;
      }
    } else {
      // Built apart, because building it may read the cell.
      const auto replacement = new_temp(*element, set.span);
      if (auto ok = lower_into(*set.value, whole(replacement)); !ok.has_value()) {
        return ok;
      }
      if (*cls != pass_class::zero_sized) {
        auto move = storage_op{.kind = op_kind::move_into};
        move.dest = target;
        move.source = whole(replacement);
        emit(std::move(move), set.span);
      }
      consumed(replacement);
    }
    if (dest.has_value()) {
      emit(storage_op{.kind = op_kind::init_empty, .dest = *dest}, set.span);
    }
    return {};
  }

  /// The operator a compound assignment applies, for the plain arithmetic and
  /// bitwise ones. Wrapping and saturating forms are not lowered yet.
  [[nodiscard]] static auto compound_operator(ast::assign_op op)
      -> std::optional<ast::binary_op> {
    switch (op) {
    case ast::assign_op::add_assign:
      return ast::binary_op::add;
    case ast::assign_op::sub_assign:
      return ast::binary_op::sub;
    case ast::assign_op::mul_assign:
      return ast::binary_op::mul;
    case ast::assign_op::div_assign:
      return ast::binary_op::div;
    case ast::assign_op::mod_assign:
      return ast::binary_op::mod;
    case ast::assign_op::and_assign:
      return ast::binary_op::bit_and;
    case ast::assign_op::or_assign:
      return ast::binary_op::bit_or;
    case ast::assign_op::xor_assign:
      return ast::binary_op::bit_xor;
    case ast::assign_op::shl_assign:
      return ast::binary_op::shl;
    case ast::assign_op::shr_assign:
      return ast::binary_op::shr;
    default:
      return std::nullopt;
    }
  }

  /// `target op= value` on a scalar: one computation that reads the target
  /// and writes it back.
  [[nodiscard]] auto lower_compound_assign(const hir_assign &assign) -> result {
    const auto op = compound_operator(assign.op);
    if (!op.has_value()) {
      return refuse(assign.span, "a wrapping or saturating compound assignment");
    }
    auto target = lower_place(*assign.target);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    if (!target->has_value()) {
      return malformed(assign.span, "the target of an assignment is not a place");
    }
    const auto &where = **target;
    if (auto ok = require_scalar_dest(where, assign.span); !ok.has_value()) {
      return ok;
    }
    // The target is read before the value runs, as in `target = target op value`.
    auto current = read_of(where, false);
    if (!is_pure(*assign.value)) {
      const auto held = new_temp(where.type, assign.span);
      auto copy = storage_op{.kind = op_kind::compute};
      copy.dest = whole(held);
      copy.computation = compute_kind::copy;
      copy.operands.push_back(current);
      emit(std::move(copy), assign.span);
      current = read_of(whole(held), false);
    }
    auto value = lower_operand(*assign.value, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    auto compute = storage_op{.kind = op_kind::compute};
    compute.dest = where;
    compute.computation = compute_kind::binary;
    compute.binary = *op;
    compute.operands.push_back(std::move(current));
    compute.operands.push_back(std::move(*value));
    emit(std::move(compute), assign.span);
    if (auto ended = flush_pending(assign.span); !ended.has_value()) {
      return ended;
    }
    return {};
  }

  /// Builds a returned value in `dest`. HIR lowers the failure arm of `x?` to
  /// a return of the operand itself, which is the function's own result type
  /// only when the success types agree; otherwise the failing variant is
  /// rebuilt in `dest` from the operand's payloads.
  [[nodiscard]] auto lower_returned(const hir_expr &value, const place &dest)
      -> result {
    if (value.kind != hir_node_kind::hir_local_ref ||
        as<hir_local_ref>(value).name != "<try subject>" ||
        value.type == dest.type) {
      return lower_into(value, dest);
    }
    const auto &entry = types().entry(value.type);
    const auto failure = entry.name == "result" ? "err" : "none";
    const auto from = layout_of(value.type, value.span);
    const auto to = layout_of(dest.type, value.span);
    if (!from.has_value()) {
      return std::unexpected(from.error());
    }
    if (!to.has_value()) {
      return std::unexpected(to.error());
    }
    const auto *source_variant = (*from)->find_variant(failure);
    const auto *dest_variant = (*to)->find_variant(failure);
    if (source_variant == nullptr || dest_variant == nullptr ||
        source_variant->payload.size() != dest_variant->payload.size()) {
      return malformed(value.span, "`?` returns `" + types().display(dest.type) +
                                       "` from `" + types().display(value.type) +
                                       "`, which have no common failure");
    }
    auto source = lower_place(value);
    if (!source.has_value()) {
      return std::unexpected(source.error());
    }
    for (size_t i = 0; i < source_variant->payload.size(); ++i) {
      const auto step = [&](const std::string &name) {
        return projection{
            .kind = projection_kind::payload, .index = i, .name = name};
      };
      if (auto ok = transfer(project(**source, step(failure),
                                     source_variant->payload[i].type),
                             project(dest, step(failure),
                                     dest_variant->payload[i].type),
                             value.span);
          !ok.has_value()) {
        return ok;
      }
    }
    auto op = storage_op{.kind = op_kind::publish_variant};
    op.dest = dest;
    op.variant = failure;
    emit(std::move(op), value.span);
    return {};
  }

  [[nodiscard]] auto lower_return(const hir_return &ret) -> result {
    const auto &signature = out_.signature;
    // A return inside a value under construction (the failure arm of `?`)
    // drops what that value has built so far.
    if (!partials_.empty()) {
      if (auto ok = drop_partials(ret.span); !ok.has_value()) {
        return ok;
      }
    }
    if (ret.value == nullptr) {
      if (auto ended = flush_pending(ret.span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, ret.span);
      return {};
    }
    return lower_return_value(*ret.value, ret.span);
  }

  /// Returns the value of `expr` from the function: builds it in the result
  /// storage, or reads it as the returned operand. The body's trailing
  /// expression statement takes this path too.
  [[nodiscard]] auto lower_return_value(const hir_expr &expr,
                                        const source_span &span) -> result {
    const auto &signature = out_.signature;
    if (signature.returns_through_address()) {
      if (auto ok = lower_returned(expr, whole(out_.result_slot));
          !ok.has_value()) {
        return ok;
      }
      if (terminated_) {
        return {}; // the value does not return
      }
      if (auto ended = flush_pending(span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, span);
      return {};
    }
    if (signature.result.cls == pass_class::zero_sized) {
      const auto temp = new_temp(expr.type, expr.span);
      if (auto ok = lower_into(expr, whole(temp)); !ok.has_value()) {
        return ok;
      }
      if (terminated_) {
        return {};
      }
      if (auto ended = flush_pending(span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, span);
      return {};
    }
    if (is_never(expr.type)) {
      return lower_discarded(expr);
    }
    auto value = lower_operand(expr, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    if (terminated_) {
      return {};
    }
    // A `*mut T` returned as a `*T` is the same address, retyped.
    const auto is_raw_pointer = [this](type_id id) -> bool {
      return types().entry(types().strip_refinement(id)).kind ==
             semantic::type_kind::ptr_kind;
    };
    if (types().strip_refinement(value->type) !=
            types().strip_refinement(signature.result.type) &&
        is_raw_pointer(value->type) && is_raw_pointer(signature.result.type)) {
      const auto temp = new_temp(signature.result.type, expr.span);
      auto op = storage_op{.kind = op_kind::compute};
      op.dest = whole(temp);
      op.computation = compute_kind::copy;
      op.operands.push_back(std::move(*value));
      emit(std::move(op), expr.span);
      value = read_of(whole(temp), false);
    }
    // The value must outlive the statement's temporaries, which end first.
    // A scalar temporary that is itself the value is simply kept; a value read
    // from inside a temporary (through a reference it holds) is copied out.
    auto reads_temporary = false;
    if (value->kind != operand_kind::constant) {
      for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->slot != value->where.base) {
          continue;
        }
        reads_temporary = true;
        if (value->where.steps.empty()) {
          pending_.erase(it);
          reads_temporary = false;
        }
        break;
      }
    }
    if (reads_temporary) {
      const auto held = new_slot(storage_class::frame_local, expr.type, "",
                                 k_invalid_symbol_id, span, false);
      if (value->kind == operand_kind::read) {
        if (auto ok = transfer(value->where, whole(held), span);
            !ok.has_value()) {
          return ok;
        }
      } else {
        auto op = storage_op{.kind = op_kind::compute};
        op.dest = whole(held);
        op.computation = compute_kind::copy;
        op.operands.push_back(*value);
        emit(std::move(op), span);
      }
      value = read_of(whole(held), value->consume);
    }
    if (auto ended = flush_pending(span); !ended.has_value()) {
      return ended;
    }
    emit_return(std::move(*value), span);
    return {};
  }

  body_shape fn_;
  storage_env &env_;
  closure_sink &closures_;
  size_t closure_count_ = 0;
  /// A step only: the coroutine it was handed, the suspension points placed so
  /// far, and the size and alignment of the state block its slots lay out.
  storage_id object_slot_ = k_invalid_storage;
  size_t suspensions_ = 0;
  size_t state_size_ = 0;
  size_t state_align_ = 1;
  storage_function out_;
  std::unordered_map<symbol_id, storage_id> locals_;
  /// The `self` this function owns (`body_shape::owns_receiver`): its symbol,
  /// which is not in `locals_`, and its parameter slot, which holds the `T`.
  struct receiver {
    symbol_id symbol = k_invalid_symbol_id;
    storage_id slot = 0;
  };
  std::optional<receiver> owned_receiver_;
  /// A pattern subject that is a part of a value: the place it names.
  std::unordered_map<symbol_id, place> subjects_;
  std::unordered_map<std::string, storage_id> globals_;
  std::vector<pending_temp> pending_;
  /// Temporaries of enclosing expressions, set aside while a nested region
  /// (a branch or a loop) lowers.
  size_t outer_pending_ = 0;
  /// The computed subjects of the `match`es being lowered, each with the
  /// number of loop frames open where it was made.
  struct open_subject {
    storage_id slot = 0;
    size_t frames = 0;
  };
  std::vector<open_subject> open_subjects_;
  /// The finished parts of aggregates still under construction whose type
  /// needs a drop, oldest first. An exit from inside the construction drops
  /// them, since nothing else owns them yet.
  std::vector<place> partials_;
  /// Whether the path being lowered has left the function or the loop body.
  bool terminated_ = false;

  /// An open region that an `exit` can leave: a loop, or the block that holds
  /// its body. `reached` records that some `exit` targets it.
  struct loop_frame {
    loop_frame_kind kind = loop_frame_kind::loop;
    bool reached = false;
    /// How many parts of values under construction were live at the loop.
    size_t partials = 0;
  };
  std::vector<loop_frame> frames_;
  /// Per open loop, the slots its body declares.
  std::vector<std::vector<storage_id>> scopes_;
};

} // namespace

namespace {

/// A `generator def` becomes two functions: the step, whose frame is the state
/// block, and the constructor, which allocates the block, moves the parameters
/// into it, and returns the coroutine.
[[nodiscard]] auto lower_generator(const hir_function &function, storage_env &env,
                                   closure_sink &closures)
    -> std::expected<storage_function, storage_lower_error> {
  const auto step_name = function.name + "$step";
  auto stepper = function_lowerer(
      body_shape{.name = step_name,
                 .params = &function.params,
                 .return_type = function.return_type,
                 .body = function.body.get(),
                 .span = function.span,
                 .step = true,
                 .generator = function.return_type},
      env, closures);
  auto step = stepper.run();
  if (!step.has_value()) {
    return std::unexpected(step.error());
  }

  auto ctor = storage_function{};
  ctor.name = function.name;
  auto param_types = std::vector<type_id>{};
  for (const auto &param : function.params) {
    param_types.push_back(param.type);
  }
  auto signature = classify_signature(env.layouts, param_types, function.return_type);
  if (!signature.has_value()) {
    return std::unexpected(storage_lower_error{
        .kind = storage_lower_error_kind::layout_failed,
        .span = function.span,
        .message = signature.error().message});
  }
  ctor.signature = std::move(*signature);
  auto make = storage_op{.kind = op_kind::make_generator};
  make.span = function.span;
  make.entry = step_name;
  make.env_size = stepper.state_size();
  make.env_align = stepper.state_align();
  // The step's parameter slots come first among its state-block slots.
  auto state_slot = size_t{0};
  const auto next_state_slot = [&]() -> const storage_slot & {
    while (step->slots[state_slot].cls != storage_class::coroutine_frame) {
      ++state_slot;
    }
    return step->slots[state_slot++];
  };
  for (const auto &param : function.params) {
    const auto id = static_cast<storage_id>(ctor.slots.size());
    ctor.slots.push_back(storage_slot{.id = id,
                                      .cls = storage_class::parameter,
                                      .type = param.type,
                                      .name = param.name,
                                      .symbol = param.symbol,
                                      .span = function.span,
                                      .initially_initialized = true,
                                      .owner = id,
                                      .owns_nothing =
                                          param.lent && !env.is_copy(param.type)});
    ctor.param_slots.push_back(id);
    const auto &held = next_state_slot();
    const auto layout = env.layouts.layout(param.type);
    if (!layout.has_value()) {
      return std::unexpected(storage_lower_error{
          .kind = storage_lower_error_kind::layout_failed,
          .span = function.span,
          .message = layout.error().message});
    }
    if ((*layout)->size == 0) {
      continue; // Nothing to store; the step's slot has no bytes either.
    }
    make.captures.push_back(closure_capture{
        .source = place{.base = id, .steps = {}, .type = param.type},
        .kind = env.is_copy(param.type) ? capture_kind::copy : capture_kind::move,
        .offset = held.env_offset});
  }
  const auto handle = static_cast<storage_id>(ctor.slots.size());
  ctor.slots.push_back(storage_slot{.id = handle,
                                    .cls = storage_class::frame_local,
                                    .type = function.return_type,
                                    .span = function.span,
                                    .owner = handle});
  make.dest = place{.base = handle, .steps = {}, .type = function.return_type};
  ctor.ops.push_back(std::move(make));
  auto ret = storage_op{.kind = op_kind::return_value};
  ret.span = function.span;
  ret.value = operand{.kind = operand_kind::read,
                      .where = place{.base = handle, .steps = {}, .type = function.return_type},
                      .consume = true,
                      .type = function.return_type};
  ctor.ops.push_back(std::move(ret));
  closures.functions.insert(closures.functions.begin(), std::move(*step));
  return ctor;
}

} // namespace

auto lower_to_storage(const hir_module &module, storage_env &env,
                      const storage_lower_options &options) -> storage_lowering {
  auto out = storage_lowering{};
  out.module.name = module.module_name;
  if (!module.statics.empty()) {
    static const auto no_params = std::vector<hir_param>{};
    auto shape = body_shape{.name = std::string{k_static_init_name},
                            .params = &no_params,
                            .return_type = env.checked.types.builtin("unit"),
                            .span = module.span,
                            .statics = &module.statics};
    for (const auto &global : module.statics) {
      out.module.globals.push_back(storage_global{
          .name = global.name, .type = global.type, .span = global.value->span});
    }
    auto closures = closure_sink{};
    auto lowered = function_lowerer(std::move(shape), env, closures).run();
    if (lowered.has_value()) {
      out.module.functions.push_back(std::move(*lowered));
      for (auto &closure : closures.functions) {
        out.module.functions.push_back(std::move(closure));
      }
    } else {
      out.refused.push_back(refused_function{
          .name = std::string{k_static_init_name},
          .error = std::move(lowered.error())});
    }
  }
  for (const auto &function : module.functions) {
    auto closures = closure_sink{};
    if (function->is_generator() || function->is_task()) {
      auto lowered = lower_generator(*function, env, closures);
      if (lowered.has_value()) {
        out.module.functions.push_back(std::move(*lowered));
        for (auto &part : closures.functions) {
          out.module.functions.push_back(std::move(part));
        }
      } else {
        out.refused.push_back(refused_function{
            .name = function->name, .error = std::move(lowered.error())});
      }
      continue;
    }
    auto lowered = function_lowerer(shape_of(*function), env, closures).run();
    if (lowered.has_value()) {
      out.module.functions.push_back(std::move(*lowered));
      for (auto &closure : closures.functions) {
        out.module.functions.push_back(std::move(closure));
      }
    } else {
      out.refused.push_back(refused_function{.name = function->name,
                                             .error = std::move(lowered.error())});
    }
  }
  if (options.nrvo) {
    for (auto &function : out.module.functions) {
      for (auto &decision : apply_nrvo(function)) {
        out.nrvo.push_back(std::move(decision));
      }
    }
  }
  // After NRVO, which can change what follows a call.
  for (auto &function : out.module.functions) {
    for (size_t i = 0; i < function.ops.size(); ++i) {
      auto &op = function.ops[i];
      if (op.kind == op_kind::call_into && op.tail_call) {
        op.tail_call = is_safe_tail_call(function, i);
      }
    }
  }
  // The frame check comes last, so that it counts what the backends get.
  auto kept = std::vector<storage_function>{};
  kept.reserve(out.module.functions.size());
  for (auto &function : out.module.functions) {
    auto plan = plan_frame(function, env);
    if (!plan.has_value()) {
      out.refused.push_back(refused_function{
          .name = function.name,
          .error = storage_lower_error{.kind = storage_lower_error_kind::layout_failed,
                                       .span = {},
                                       .message = plan.error()}});
      continue;
    }
    if (!plan->within(options.max_frame_bytes)) {
      const auto largest = std::ranges::max_element(
          plan->contributors, {}, &frame_contributor::bytes);
      out.refused.push_back(refused_function{
          .name = function.name,
          .error = storage_lower_error{
              .kind = storage_lower_error_kind::frame_too_large,
              .span = largest == plan->contributors.end() ? source_span{}
                                                          : largest->span,
              .message = describe_frame_overflow(*plan, options.max_frame_bytes)}});
      continue;
    }
    out.frames.push_back(std::move(*plan));
    kept.push_back(std::move(function));
  }
  out.module.functions = std::move(kept);
  return out;
}

} // namespace cinder::hir
