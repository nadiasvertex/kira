#include "src/hir/storage_lower.h"

#include "src/hir/captures.h"
#include "src/hir/traversal.h"

#include <algorithm>
#include <charconv>
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
  /// A closure body: the environment it reads its captures from.
  const environment_plan *environment = nullptr;
  /// A closure's drop glue: releases the environment block when it ends.
  bool releases_environment = false;
};

/// The closure bodies and drop glue that lowering one function produced.
struct closure_sink {
  std::vector<storage_function> functions;
};

[[nodiscard]] auto shape_of(const hir_function &function) -> body_shape {
  return body_shape{.name = function.name,
                    .params = &function.params,
                    .return_type = function.return_type,
                    .body = function.body.get(),
                    .span = function.span,
                    .coroutine = function.is_coroutine()};
}

class function_lowerer {
public:
  function_lowerer(body_shape shape, storage_env &env, closure_sink &closures)
      : fn_(std::move(shape)), env_(env), closures_(closures) {}

  [[nodiscard]] auto run() -> std::expected<storage_function, storage_lower_error> {
    if (fn_.coroutine) {
      return refuse(fn_.span,
                    "a generator or task body needs suspension-aware storage");
    }
    out_.name = fn_.name;
    auto param_types = std::vector<type_id>{};
    for (const auto &param : *fn_.params) {
      param_types.push_back(param.type);
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
    for (const auto &param : *fn_.params) {
      const auto id = new_slot(storage_class::parameter, param.type, param.name,
                               param.symbol, fn_.span, true);
      out_.param_slots.push_back(id);
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
    auto ret = storage_op{.kind = op_kind::return_value};
    ret.value = std::move(value);
    emit(std::move(ret), span);
    terminated_ = true;
  }

  [[nodiscard]] auto types() const -> const semantic::type_table & {
    return env_.types();
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
      const auto found = locals_.find(as<hir_local_ref>(expr).symbol);
      if (found == locals_.end()) {
        return std::optional<place>{};
      }
      return std::optional<place>{whole(found->second)};
    }
    case hir_node_kind::hir_global_ref: {
      const auto &name = as<hir_global_ref>(expr).name;
      auto found = globals_.find(name);
      if (found == globals_.end()) {
        const auto id = new_slot(storage_class::global, expr.type, name,
                                 k_invalid_symbol_id, expr.span, true);
        found = globals_.emplace(name, id).first;
      }
      return std::optional<place>{whole(found->second)};
    }
    case hir_node_kind::hir_field: {
      const auto &field = as<hir_field>(expr);
      auto base = lower_place(*field.object);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      if (!base->has_value()) {
        return refuse(expr.span, "a field of a computed value");
      }
      auto object = auto_deref(std::move(**base));
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
      auto base = lower_place(*index.object);
      if (!base.has_value()) {
        return std::unexpected(base.error());
      }
      if (!base->has_value()) {
        return refuse(expr.span, "an element of a computed value");
      }
      auto object = auto_deref(std::move(**base));
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
      const auto &entry =
          types().entry(types().strip_refinement(pointer.type));
      if (entry.kind != semantic::type_kind::ref_kind &&
          entry.kind != semantic::type_kind::ptr_kind) {
        return malformed(expr.span, "cannot dereference `" +
                                        types().display(pointer.type) + "`");
      }
      return std::optional<place>{project(
          std::move(pointer), projection{.kind = projection_kind::deref},
          entry.result)};
    }
    default:
      return std::optional<place>{};
    }
  }

  /// `x[i]`: a slot of a raw pointer, or an element of an array at a constant
  /// index. Any other indexing is not a place the form names yet.
  [[nodiscard]] auto lower_index(const hir_index &index)
      -> std::expected<std::optional<place>, storage_lower_error> {
    if (indexes_raw_pointer(index)) {
      return lower_raw_index(index);
    }
    if (index.index->kind != hir_node_kind::hir_literal) {
      return std::optional<place>{};
    }
    const auto &literal = as<hir_literal>(*index.index);
    auto at = size_t{0};
    const auto *end = literal.value.data() + literal.value.size();
    if (literal.lit_kind != token_kind::int_lit ||
        std::from_chars(literal.value.data(), end, at).ptr != end) {
      return std::optional<place>{};
    }
    auto base = lower_place(*index.object);
    if (!base.has_value()) {
      return std::unexpected(base.error());
    }
    if (!base->has_value()) {
      return refuse(index.span, "an element of a computed value");
    }
    auto object = auto_deref(std::move(**base));
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
  [[nodiscard]] auto lower_operand(const hir_expr &expr, bool later_effects)
      -> std::expected<operand, storage_lower_error> {
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
      const auto consume = consumes(*cls, expr.type);
      if (!later_effects) {
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
    const auto consume = consumes(*cls, expr.type);
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
    switch (expr.kind) {
    case hir_node_kind::hir_literal:
      return lower_literal(as<hir_literal>(expr), dest);
    case hir_node_kind::hir_local_ref:
    case hir_node_kind::hir_global_ref:
    case hir_node_kind::hir_field:
    case hir_node_kind::hir_tuple_index:
    case hir_node_kind::hir_variant_payload:
      return transfer_from_place(expr, dest);
    case hir_node_kind::hir_index: {
      auto where = lower_place(expr);
      if (!where.has_value()) {
        return std::unexpected(where.error());
      }
      if (!where->has_value()) {
        return refuse(expr.span,
                      "indexing other than a raw pointer or an array at a "
                      "constant index");
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
      return lower_block(as<hir_block>(expr), dest);
    case hir_node_kind::hir_if:
      return lower_if(as<hir_if>(expr), dest);
    case hir_node_kind::hir_lambda:
      return lower_lambda(as<hir_lambda>(expr), dest);
    case hir_node_kind::hir_closure_drop:
      return lower_closure_drop(as<hir_closure_drop>(expr), dest);
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
      return refuse(expr.span, "a function name used as a value");
    }
    return transfer(**source, dest, expr.span);
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
    for (size_t i = 0; i < tuple.elements.size(); ++i) {
      const auto part =
          project(dest, member_step(i, ""), found.members[i].type);
      if (auto ok = lower_into(*tuple.elements[i], part); !ok.has_value()) {
        return ok;
      }
    }
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
    }
    return {};
  }

  [[nodiscard]] auto lower_array(const hir_array_init &init, const place &dest)
      -> result {
    if (init.fill_value != nullptr) {
      return refuse(init.span, "an array fill `[value; count]`, which needs a "
                               "loop");
    }
    const auto layout = layout_of(dest.type, init.span);
    if (!layout.has_value()) {
      return std::unexpected(layout.error());
    }
    const auto &found = **layout;
    if (found.kind != runtime::value_layout_kind::array ||
        found.count != init.elements.size()) {
      return refuse(init.span, "an array literal that does not build a fixed "
                               "`array` of its own length");
    }
    if (init.elements.empty()) {
      return empty_value(dest, init.span);
    }
    for (size_t i = 0; i < init.elements.size(); ++i) {
      const auto part = project(
          dest, projection{.kind = projection_kind::element, .index = i},
          found.element);
      if (auto ok = lower_into(*init.elements[i], part); !ok.has_value()) {
        return ok;
      }
    }
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
    }
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
  [[nodiscard]] auto tail_value(const hir_block &block, const hir_node &stmt) const
      -> const hir_expr * {
    if (stmt.kind != hir_node_kind::hir_expr_stmt) {
      return dynamic_cast<const hir_expr *>(&stmt);
    }
    const auto yields = block.type != semantic::k_unknown_type &&
                        !types().is_unit(block.type);
    return yields ? as<hir_expr_stmt>(stmt).expr.get() : nullptr;
  }

  /// A block runs its statements in order. When it ends in an expression,
  /// that expression is the block's value and is built in `dest`. HIR makes a
  /// discarded temporary, or a value computed before scope-exit drops, a
  /// block of this shape.
  [[nodiscard]] auto lower_block(const hir_block &block, const place &dest)
      -> result {
    for (size_t i = 0; i < block.stmts.size() && !terminated_; ++i) {
      const auto &stmt = *block.stmts[i];
      const auto last = i + 1 == block.stmts.size();
      const auto *value = last ? tail_value(block, stmt) : nullptr;
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

  [[nodiscard]] auto lower_call(const hir_call &call, const place &dest)
      -> result {
    auto params = std::vector<type_id>{};
    for (const auto &arg : call.args) {
      params.push_back(arg->type);
    }
    auto signature = classify_signature(env_.layouts, params, call.type);
    if (!signature.has_value()) {
      return layout_failure(call.span, signature.error());
    }

    auto target = call_target{};
    if (call.callee->kind == hir_node_kind::hir_local_ref &&
        !locals_.contains(as<hir_local_ref>(*call.callee).symbol)) {
      const auto &callee = as<hir_local_ref>(*call.callee);
      target.name = callee.name;
      target.owner_module = callee.owner_module;
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
    auto outgoing = std::vector<storage_id>{};
    for (size_t i = 0; i < call.args.size(); ++i) {
      const auto &arg = *call.args[i];
      const auto &param = signature->params[i];
      auto lowered = call_argument{.source_index = i, .cls = param.cls};
      if (param.cls == pass_class::zero_sized) {
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
        auto value = lower_operand(arg, later_effects);
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
    case hir_node_kind::hir_while:
      return lower_while(as<hir_while>(node));
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
    if (expr.kind == hir_node_kind::hir_closure_drop) {
      // Its `unit` type is not settled in HIR, and nothing reads the value.
      return lower_closure_drop(as<hir_closure_drop>(expr), std::nullopt);
    }
    const auto temp = new_temp(expr.type, expr.span);
    if (auto ok = lower_into(expr, whole(temp)); !ok.has_value()) {
      return ok;
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
        return lower_block(body, *dest);
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
    frames_.push_back(loop_frame{.kind = loop_frame_kind::loop});
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

    emit(storage_op{.kind = op_kind::block_begin}, loop.body->span);
    frames_.push_back(loop_frame{.kind = loop_frame_kind::body});
    auto ok = in_region(loop.body->span,
                        [&] { return lower_discarded(*loop.body); });
    if (!ok.has_value()) {
      return ok;
    }
    const auto continued = frames_.back().reached;
    frames_.pop_back();
    emit(storage_op{.kind = op_kind::block_end}, loop.body->span);
    terminated_ = terminated_ && !continued;

    if (!terminated_ && loop.step != nullptr) {
      auto stepped = in_region(loop.step->span,
                               [&] { return lower_discarded(*loop.step); });
      if (!stepped.has_value()) {
        return stepped;
      }
    }
    if (!terminated_) {
      // Storage declared in the body starts over on the next iteration.
      for (auto it = scopes_.back().rbegin(); it != scopes_.back().rend(); ++it) {
        kill(*it, loop.span);
      }
    }
    emit(storage_op{.kind = op_kind::loop_end}, loop.span);
    const auto left = frames_.back().reached;
    frames_.pop_back();
    scopes_.pop_back();
    terminated_ = !left;
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
      frame.reached = true;
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

  [[nodiscard]] auto lower_return(const hir_return &ret) -> result {
    const auto &signature = out_.signature;
    if (ret.value == nullptr) {
      if (auto ended = flush_pending(ret.span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, ret.span);
      return {};
    }
    if (signature.returns_through_address()) {
      if (auto ok = lower_into(*ret.value, whole(out_.result_slot));
          !ok.has_value()) {
        return ok;
      }
      if (auto ended = flush_pending(ret.span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, ret.span);
      return {};
    }
    if (signature.result.cls == pass_class::zero_sized) {
      const auto temp = new_temp(ret.value->type, ret.value->span);
      if (auto ok = lower_into(*ret.value, whole(temp)); !ok.has_value()) {
        return ok;
      }
      if (auto ended = flush_pending(ret.span); !ended.has_value()) {
        return ended;
      }
      emit_return(std::nullopt, ret.span);
      return {};
    }
    auto value = lower_operand(*ret.value, false);
    if (!value.has_value()) {
      return std::unexpected(value.error());
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
      const auto held = new_slot(storage_class::frame_local, ret.value->type, "",
                                 k_invalid_symbol_id, ret.span, false);
      if (value->kind == operand_kind::read) {
        if (auto ok = transfer(value->where, whole(held), ret.span);
            !ok.has_value()) {
          return ok;
        }
      } else {
        auto op = storage_op{.kind = op_kind::compute};
        op.dest = whole(held);
        op.computation = compute_kind::copy;
        op.operands.push_back(*value);
        emit(std::move(op), ret.span);
      }
      value = read_of(whole(held), value->consume);
    }
    if (auto ended = flush_pending(ret.span); !ended.has_value()) {
      return ended;
    }
    emit_return(std::move(*value), ret.span);
    return {};
  }

  body_shape fn_;
  storage_env &env_;
  closure_sink &closures_;
  size_t closure_count_ = 0;
  storage_function out_;
  std::unordered_map<symbol_id, storage_id> locals_;
  std::unordered_map<std::string, storage_id> globals_;
  std::vector<pending_temp> pending_;
  /// Temporaries of enclosing expressions, set aside while a nested region
  /// (a branch or a loop) lowers.
  size_t outer_pending_ = 0;
  /// Whether the path being lowered has left the function or the loop body.
  bool terminated_ = false;

  /// An open region that an `exit` can leave: a loop, or the block that holds
  /// its body. `reached` records that some `exit` targets it.
  struct loop_frame {
    loop_frame_kind kind = loop_frame_kind::loop;
    bool reached = false;
  };
  std::vector<loop_frame> frames_;
  /// Per open loop, the slots its body declares.
  std::vector<std::vector<storage_id>> scopes_;
};

} // namespace

auto lower_to_storage(const hir_module &module, storage_env &env)
    -> storage_lowering {
  auto out = storage_lowering{};
  out.module.name = module.module_name;
  for (const auto &function : module.functions) {
    auto closures = closure_sink{};
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
  return out;
}

} // namespace cinder::hir
