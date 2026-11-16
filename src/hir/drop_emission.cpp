#include "src/hir/drop_emission.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace cinder::hir {

namespace {

using semantic::k_unknown_type;
using semantic::type_kind;

[[nodiscard]] auto fail(lowering_error_kind kind, source_span span,
                        std::string message)
    -> std::unexpected<lowering_error> {
  return std::unexpected(lowering_error{
      .kind = kind, .span = span, .message = std::move(message)});
}

/// A direct call to `decl`, recording it as the call's `target`.
[[nodiscard]] auto direct_call(source_span span, type_id type,
                               ptr<hir_expr> callee, ptr_vec<hir_expr> args,
                               const ast::func_decl *decl) -> ptr<hir_call> {
  auto call = ptr<hir_call>(
      make<hir_call>(span, type, std::move(callee), std::move(args)));
  call->target = decl;
  return call;
}

} // namespace

auto always_returns(const hir_node &node) -> bool {
  switch (node.kind) {
  case hir_node_kind::hir_return:
    return true;
  case hir_node_kind::hir_expr_stmt:
    return always_returns(*dynamic_cast<const hir_expr_stmt &>(node).expr);
  case hir_node_kind::hir_block: {
    const auto &block = dynamic_cast<const hir_block &>(node);
    return std::ranges::any_of(block.stmts, [](const auto &stmt) -> bool {
      return always_returns(*stmt);
    });
  }
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir_if &>(node);
    // Without an `else`, the condition can simply be false, and control
    // arrives at whatever follows.
    return node2.else_body != nullptr && always_returns(*node2.else_body) &&
           std::ranges::all_of(node2.branches, [](const auto &branch) -> bool {
             return always_returns(*branch.body);
           });
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir_match &>(node);
    return std::ranges::all_of(node2.arms, [](const auto &arm) -> bool {
      return always_returns(*arm.body);
    });
  }
  default:
    return false;
  }
}

auto always_exits(const hir_node &node) -> bool {
  switch (node.kind) {
  case hir_node_kind::hir_return:
  case hir_node_kind::hir_break:
  case hir_node_kind::hir_continue:
    return true;
  case hir_node_kind::hir_expr_stmt:
    return always_exits(*dynamic_cast<const hir_expr_stmt &>(node).expr);
  case hir_node_kind::hir_block: {
    const auto &block = dynamic_cast<const hir_block &>(node);
    return std::ranges::any_of(block.stmts, [](const auto &stmt) -> bool {
      return always_exits(*stmt);
    });
  }
  case hir_node_kind::hir_if: {
    const auto &node2 = dynamic_cast<const hir_if &>(node);
    return node2.else_body != nullptr && always_exits(*node2.else_body) &&
           std::ranges::all_of(node2.branches, [](const auto &branch) -> bool {
             return always_exits(*branch.body);
           });
  }
  case hir_node_kind::hir_match: {
    const auto &node2 = dynamic_cast<const hir_match &>(node);
    return std::ranges::all_of(node2.arms, [](const auto &arm) -> bool {
      return always_exits(*arm.body);
    });
  }
  default:
    return false;
  }
}

auto clone_place(const hir_expr &place)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  switch (place.kind) {
  case hir_node_kind::hir_local_ref: {
    const auto &ref = dynamic_cast<const hir_local_ref &>(place);
    return ptr<hir_expr>(make<hir_local_ref>(ref.span, ref.type, ref.symbol,
                                             ref.name, ref.owner_module));
  }
  case hir_node_kind::hir_field: {
    const auto &field = dynamic_cast<const hir_field &>(place);
    auto object = clone_place(*field.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    return ptr<hir_expr>(make<hir_field>(
        field.span, field.type, std::move(*object), field.field_name));
  }
  case hir_node_kind::hir_tuple_index: {
    const auto &index = dynamic_cast<const hir_tuple_index &>(place);
    auto object = clone_place(*index.object);
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    return ptr<hir_expr>(make<hir_tuple_index>(
        index.span, index.type, std::move(*object), index.index));
  }
  case hir_node_kind::hir_unary: {
    const auto &unary = dynamic_cast<const hir_unary &>(place);
    if (unary.op != ast::unary_op::deref || unary.operand == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, place.span,
                  "this place cannot be read again: only a local, a field "
                  "or tuple projection of one, or a `*` through a "
                  "reference can");
    }
    auto operand = clone_place(*unary.operand);
    if (!operand.has_value()) {
      return std::unexpected(operand.error());
    }
    return ptr<hir_expr>(hir::make<hir_unary>(unary.span, unary.type,
                                              unary.op, std::move(*operand)));
  }
  default:
    return fail(lowering_error_kind::unsupported_construct, place.span,
                "this value's type declares an `invariant`, but the "
                "expression being assigned through is not a simple place "
                "(a local, or a field/tuple projection of one), so the "
                "invariant cannot be re-checked after the write yet");
  }
}


auto owns_receiver(const semantic::checked_types &checked,
                   const ast::func_decl *decl) -> bool {
  if (decl == nullptr) {
    return false;
  }
  const auto &owners = checked.ownership.consuming_receivers;
  if (owners.contains(decl)) {
    return true;
  }
  const auto tmpl = checked.ownership.instance_templates.find(decl);
  return tmpl != checked.ownership.instance_templates.end() &&
         owners.contains(tmpl->second);
}

auto drop_emitter::start(drop_schedule schedule) -> void {
  schedule_ = std::move(schedule);
  flag_symbols_.assign(schedule_.flag_count, k_invalid_symbol_id);
}

auto drop_emitter::declared_result_type(const ast::func_decl &decl) const
    -> type_id {
  if (decl.return_type == nullptr) {
    const auto inferred = checked_.inferred_return_types.find(&decl);
    return inferred != checked_.inferred_return_types.end() &&
                   inferred->second != k_unknown_type
               ? inferred->second
               : const_cast<semantic::type_table &>(checked_.types)
                     .builtin("unit");
  }
  const auto found = checked_.node_types.find(decl.return_type.get());
  return found != checked_.node_types.end() ? found->second : k_unknown_type;
}

auto drop_emitter::clear_moved(const void *key, ptr<hir_expr> lowered)
    -> outcome<ptr<hir_expr>> {
  const auto clears = schedule_.move_clears.find(key);
  if (clears == schedule_.move_clears.end()) {
    return lowered;
  }
  // `{flag = false; value}`: the value moves, so the binding it came from
  // no longer owns it.
  const auto span = lowered->span;
  const auto type = lowered->type;
  auto stmts = ptr_vec<hir_node>{};
  if (auto cleared = set_flags(clears->second, false, span, stmts);
      !cleared.has_value()) {
    return std::unexpected(cleared.error());
  }
  stmts.push_back(ptr<hir_node>(make<hir_expr_stmt>(span, std::move(lowered))));
  auto block = make<hir_block>(span, type, std::move(stmts));
  block->origin = key;
  return ptr<hir_expr>(std::move(block));
}

auto drop_emitter::emit_jump_drops(const void *key, jump_exit kind,
                                   source_span span, ptr_vec<hir_node> &stmts)
    -> outcome<void> {
  // The temporaries the jump leaves alive drop first, innermost; the rewriter
  // that makes them (`make_references_explicit`) fills this in.
  auto placeholder = make<hir_block>(span, k_unknown_type, ptr_vec<hir_node>{});
  placeholder->exit = kind;
  stmts.push_back(ptr<hir_node>(
      make<hir_expr_stmt>(span, ptr<hir_expr>(std::move(placeholder)))));
  return emit_scope_exit_drops(key, stmts);
}

auto drop_emitter::assignment(const void *key, ast::assign_op op,
                              ptr<hir_expr> target, ptr<hir_expr> value,
                              source_span span, ptr_vec<hir_node> &stmts)
    -> outcome<void> {
  // Assigning over a value drops the old one, after the new one is
  // computed (ch. 17, When drops run): the new value goes to a temporary,
  // the old one drops, and the temporary is stored.
  const auto scheduled = op == ast::assign_op::assign
                             ? schedule_.assignments.find(key)
                             : schedule_.assignments.end();
  if (scheduled != schedule_.assignments.end() && scheduled->second.drop_old &&
      checked_.drop.drop_plans.contains(target->type)) {
    const auto temp_type = value->type;
    const auto temp_symbol = env_.mint();
    stmts.push_back(ptr<hir_node>(make<hir_let>(
        span, temp_symbol, std::string("<assigned>"), std::move(value))));
    const auto &old_place = *target;
    if (auto probe = clone_place(old_place); !probe.has_value()) {
      return std::unexpected(probe.error());
    }
    const place_fn make_place = [&old_place]() -> ptr<hir_expr> {
      return std::move(*clone_place(old_place));
    };
    auto drops = ptr_vec<hir_node>{};
    if (auto dropped = build_drop_calls(make_place, target->type, span, drops,
                                        scheduled->second.moved_paths);
        !dropped.has_value()) {
      return dropped;
    }
    // Moved on some paths: drop the old value only where it is still there.
    auto checked_drops = guarded(scheduled->second.when, std::move(drops), span);
    if (!checked_drops.has_value()) {
      return std::unexpected(checked_drops.error());
    }
    for (auto &drop : *checked_drops) {
      stmts.push_back(std::move(drop));
    }
    value = ptr<hir_expr>(make<hir_local_ref>(span, temp_type, temp_symbol,
                                              std::string("<assigned>")));
  }
  stmts.push_back(ptr<hir_node>(
      hir::make<hir_assign>(span, op, std::move(target), std::move(value))));
  // The target holds a value again.
  if (scheduled != schedule_.assignments.end()) {
    return set_flags(scheduled->second.sets, true, span, stmts);
  }
  return {};
}

auto drop_emitter::drop_temporary(const temporary &temp,
                                  ptr_vec<hir_node> &out) -> outcome<void> {
  const auto symbol = temp.symbol;
  const auto type = temp.type;
  const place_fn place = [symbol, type]() -> ptr<hir_expr> {
    return {make<hir_local_ref>(source_span::dummy(), type, symbol,
                                std::string("<temporary>"))};
  };
  auto moved = field_paths{};
  for (const auto &path : temp.moved_paths) {
    moved.push_back(moved_path{.path = path, .flag = std::nullopt});
  }
  return build_drop_calls(place, type, source_span::dummy(), out, moved);
}

auto drop_emitter::temporary_end(const void *origin) const
    -> std::optional<const void *> {
  const auto found = schedule_.temporary_ends.find(origin);
  if (found == schedule_.temporary_ends.end()) {
    return std::nullopt;
  }
  return found->second;
}

auto drop_emitter::build_array_drop(const place_fn &make_receiver,
                               const semantic::drop_plan &plan,
                               source_span span, ptr_vec<hir_node> &out,
                               const field_paths &moved_paths)
    -> outcome<void> {
  if (!moved_paths.empty()) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "internal error: an array drop was scheduled for an array "
                "that was moved out of in part");
  }
  const auto element = *plan.element;
  const auto length = plan.length;
  const auto usize_type = checked_.types.usize_type();
  const auto pointer_type = plan.element_pointer;
  const auto data_symbol = env_.mint();
  const auto index_symbol = env_.mint();
  auto data = make<hir_container_data>(span, pointer_type, make_receiver());
  data->takes_elements = true;
  out.push_back(ptr<hir_node>(make<hir_let>(span, data_symbol,
                                            std::string("<drop elements>"),
                                            ptr<hir_expr>(std::move(data)))));
  out.push_back(ptr<hir_node>(make<hir_let>(
      span, index_symbol, std::string("<drop index>"),
      ptr<hir_expr>(make<hir_literal>(span, usize_type, token_kind::int_lit,
                                      std::string("0"))),
      /*mut=*/true)));
  const place_fn element_place = [span, element, pointer_type, data_symbol,
                                  usize_type, index_symbol]() -> ptr<hir_expr> {
    return {make<hir_index>(
        span, element,
        ptr<hir_expr>(make<hir_local_ref>(span, pointer_type, data_symbol,
                                          std::string("<drop elements>"))),
        ptr<hir_expr>(make<hir_local_ref>(span, usize_type, index_symbol,
                                          std::string("<drop index>"))))};
  };
  auto body = ptr_vec<hir_node>{};
  if (auto ok = build_drop_calls(element_place, element, span, body);
      !ok.has_value()) {
    return ok;
  }
  auto step = ptr_vec<hir_node>{};
  step.push_back(ptr<hir_node>(hir::make<hir_assign>(
      span, ast::assign_op::add_assign,
      ptr<hir_expr>(make<hir_local_ref>(span, usize_type, index_symbol,
                                        std::string("<drop index>"))),
      ptr<hir_expr>(make<hir_literal>(span, usize_type, token_kind::int_lit,
                                      std::string("1"))))));
  auto condition = ptr<hir_expr>(hir::make<hir_binary>(
      span, checked_.types.bool_type(), ast::binary_op::lt,
      ptr<hir_expr>(make<hir_local_ref>(span, usize_type, index_symbol,
                                        std::string("<drop index>"))),
      ptr<hir_expr>(make<hir_literal>(span, usize_type, token_kind::int_lit,
                                      std::to_string(length)))));
  out.push_back(ptr<hir_node>(
      make<hir_while>(span, std::move(condition),
                      make<hir_block>(span, k_unknown_type, std::move(body)),
                      make<hir_block>(span, k_unknown_type, std::move(step)))));
  return {};
}

auto drop_emitter::build_drop_calls(const place_fn &make_receiver, type_id type,
                               source_span span, ptr_vec<hir_node> &out,
                               const field_paths &moved_paths)
    -> outcome<void> {
  const auto found = checked_.drop.drop_plans.find(type);
  if (found == checked_.drop.drop_plans.end()) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "internal error: scope-exit drop was scheduled for a type "
                "with no resolved drop plan — the drop-schedule pass and "
                "checker::resolve_drop_plans have gotten out of sync");
  }
  const auto &plan = found->second;
  if (plan.cancels_generator) {
    out.push_back(ptr<hir_node>(make<hir_expr_stmt>(
        span, ptr<hir_expr>(make<hir_coroutine_cancel>(span, k_unknown_type,
                                                       make_receiver())))));
    return {};
  }
  if (plan.closure_glue.has_value()) {
    out.push_back(ptr<hir_node>(make<hir_expr_stmt>(
        span, ptr<hir_expr>(make<hir_closure_drop>(span, k_unknown_type,
                                                   make_receiver())))));
    return {};
  }
  if (plan.element.has_value()) {
    return build_array_drop(make_receiver, plan, span, out, moved_paths);
  }
  if (plan.own_drop.has_value()) {
    const auto &resolved = *plan.own_drop;
    // Same convention as `lower_index_dispatch` just above: an empty
    // `impl_target_type` means `decl` is already a monomorphized instance
    // carrying its own mangled name.
    const auto local_name =
        resolved.impl_target_type.empty()
            ? resolved.decl->name
            : std::format("{}::{}", resolved.impl_target_type,
                          resolved.decl->name);
    const auto symbol = env_.function_symbol(local_name);
    auto callee = ptr<hir_expr>(make<hir_local_ref>(
        span, k_unknown_type, symbol, local_name, resolved.owner_module));
    auto args = ptr_vec<hir_expr>{};
    args.push_back(make_receiver());
    const auto result_type = declared_result_type(*resolved.decl);
    auto own_drop = direct_call(span, result_type, std::move(callee),
                                std::move(args), resolved.decl);
    own_drop->is_drop = true;
    out.push_back(ptr<hir_node>(
        make<hir_expr_stmt>(span, ptr<hir_expr>(std::move(own_drop)))));
  }
  // Field-wise, in declaration order, regardless of whether this type had
  // its own `impl drop` above — the spec's implicit field-wise rule applies
  // either way (`drop_plan`'s doc comment, types.h).
  // A field moved out whole is skipped; one moved out of in part drops
  // what it still owns.
  for (const auto &[field_name, field_type] : plan.droppable_fields) {
    auto moved_whole = false;
    auto whole_flag = std::optional<std::size_t>{};
    auto moved_below = field_paths{};
    for (const auto &moved : moved_paths) {
      if (moved.path.empty() || moved.path.front() != field_name) {
        continue;
      }
      if (moved.path.size() == 1) {
        moved_whole = !moved.flag.has_value();
        whole_flag = moved.flag;
      } else {
        moved_below.push_back(
            moved_path{.path = std::vector<std::string>(moved.path.begin() + 1,
                                                        moved.path.end()),
                       .flag = moved.flag});
      }
    }
    if (moved_whole) {
      continue;
    }
    // A tuple's members are named by position.
    const auto position =
        checked_.types.entry(type).kind == semantic::type_kind::tuple_kind
            ? semantic::tuple_index_of(field_name)
            : std::nullopt;
    const place_fn field_place = [make_receiver, field_name, field_type,
                                  position]() -> ptr<hir_expr> {
      if (position.has_value()) {
        return {make<hir_tuple_index>(source_span::dummy(), field_type,
                                      make_receiver(), *position)};
      }
      return {make<hir_field>(source_span::dummy(), field_type, make_receiver(),
                              field_name)};
    };
    // A field moved out on some paths only drops when its flag says it is
    // still there.
    auto field_drops = ptr_vec<hir_node>{};
    if (auto sub = build_drop_calls(field_place, field_type, span, field_drops,
                                    moved_below);
        !sub.has_value()) {
      return sub;
    }
    auto flags = std::vector<std::size_t>{};
    if (whole_flag.has_value()) {
      flags.push_back(*whole_flag);
    }
    auto checked_drops = guarded(flags, std::move(field_drops), span);
    if (!checked_drops.has_value()) {
      return std::unexpected(checked_drops.error());
    }
    for (auto &drop : *checked_drops) {
      out.push_back(std::move(drop));
    }
  }
  if (!plan.variant_drops.empty()) {
    // A sum value: match on the active variant and drop only its payload.
    // The subject is the place itself, so nothing is copied out first.
    const auto subject_symbol = env_.mint();
    const place_fn subject_place = [type, subject_symbol]() -> ptr<hir_expr> {
      return {make<hir_local_ref>(source_span::dummy(), type, subject_symbol,
                                  std::string("<drop subject>"))};
    };
    auto arms = std::vector<hir_match_arm>{};
    for (const auto &vd : plan.variant_drops) {
      auto body_stmts = ptr_vec<hir_node>{};
      for (const auto &[index, payload_type] : vd.droppable_payloads) {
        const auto variant_name = vd.variant;
        const auto slot = index;
        const place_fn payload_place = [subject_place, variant_name, slot,
                                        payload_type]() -> ptr<hir_expr> {
          return {make<hir_variant_payload>(source_span::dummy(), payload_type,
                                            subject_place(), variant_name,
                                            slot)};
        };
        if (auto sub =
                build_drop_calls(payload_place, payload_type, span, body_stmts);
            !sub.has_value()) {
          return sub;
        }
      }
      auto pattern_args = ptr_vec<hir_pattern>{};
      for (size_t i = 0; i < vd.arity; ++i) {
        pattern_args.push_back(
            ptr<hir_pattern>(make<hir_wildcard_pattern>(span)));
      }
      auto pattern = ptr<hir_pattern>(make<hir_constructor_pattern>(
          span, vd.variant, std::move(pattern_args)));
      pattern->subject_type = type;
      arms.push_back(
          hir_match_arm{.pattern = std::move(pattern),
                        .guard = nullptr,
                        .body = make<hir_block>(span, k_unknown_type,
                                                std::move(body_stmts))});
    }
    arms.push_back(hir_match_arm{
        .pattern = ptr<hir_pattern>(make<hir_wildcard_pattern>(span)),
        .guard = nullptr,
        .body = make<hir_block>(span, k_unknown_type, ptr_vec<hir_node>{})});
    auto match =
        ptr<hir_expr>(make<hir_match>(span, k_unknown_type, make_receiver(),
                                      subject_symbol, std::move(arms)));
    out.push_back(ptr<hir_node>(make<hir_expr_stmt>(span, std::move(match))));
  }
  return {};
}

auto drop_emitter::emit_one_drop(const pending_drop &drop, ptr_vec<hir_node> &stmts)
    -> outcome<void> {
  auto drops = ptr_vec<hir_node>{};
  {
    const auto symbol = env_.whole_local(drop.node);
    if (!symbol.has_value()) {
      return fail(lowering_error_kind::unsupported_construct,
                  source_span::dummy(),
                  std::format("internal error: drop schedule named `{}`, "
                              "which is not a local in scope at this point",
                              drop.name));
    }
    const auto drop_symbol = *symbol;
    const auto drop_type = drop.type;
    const auto drop_name = drop.name;
    const place_fn make_receiver = [drop_symbol, drop_type,
                                    drop_name]() -> ptr<hir_expr> {
      return {make<hir_local_ref>(source_span::dummy(), drop_type, drop_symbol,
                                  drop_name)};
    };
    if (auto built =
            build_drop_calls(make_receiver, drop.type, source_span::dummy(),
                             drops, drop.moved_paths);
        !built.has_value()) {
      return built;
    }
  }
  // Moved on some paths to this exit only: drop it when its flag says it is
  // still here.
  auto flags = std::vector<std::size_t>{};
  if (drop.flag.has_value()) {
    flags.push_back(*drop.flag);
  }
  auto checked_drops = guarded(flags, std::move(drops), source_span::dummy());
  if (!checked_drops.has_value()) {
    return std::unexpected(checked_drops.error());
  }
  for (auto &node : *checked_drops) {
    stmts.push_back(std::move(node));
  }
  return {};
}

auto drop_emitter::emit_scope_exit_drops(const void *key, ptr_vec<hir_node> &stmts)
    -> outcome<void> {
  const auto found = schedule_.exits.find(key);
  if (found == schedule_.exits.end()) {
    return {};
  }
  for (const auto &drop : found->second) {
    if (auto result = emit_one_drop(drop, stmts); !result.has_value()) {
      return result;
    }
  }
  return {};
}

auto drop_emitter::emit_tail_scope_exit_drops(const void *key,
                                         ptr_vec<hir_node> &stmts)
    -> outcome<void> {
  const auto scheduled = schedule_.exits.find(key);
  if (scheduled == schedule_.exits.end() || scheduled->second.empty()) {
    return {};
  }
  const auto has_value_tail =
      !stmts.empty() && stmts.back()->kind == hir_node_kind::hir_expr_stmt &&
      [&] {
        const auto &tail = dynamic_cast<const hir_expr_stmt &>(*stmts.back());
        return tail.expr->type != k_unknown_type &&
               !checked_.types.is_unit(tail.expr->type);
      }();
  if (!has_value_tail) {
    return emit_scope_exit_drops(key, stmts);
  }
  auto tail = std::move(dynamic_cast<hir_expr_stmt &>(*stmts.back()).expr);
  const auto tail_span = tail->span;
  const auto tail_type = tail->type;
  stmts.pop_back();
  const auto temp = env_.mint();
  const auto temp_name = std::format("<tail value {}>", temp);
  stmts.push_back(ptr<hir_node>(
      make<hir_let>(tail_span, temp, temp_name, std::move(tail))));
  if (auto result = emit_scope_exit_drops(key, stmts); !result.has_value()) {
    return result;
  }
  stmts.push_back(ptr<hir_node>(make<hir_expr_stmt>(
      tail_span, ptr<hir_expr>(make<hir_local_ref>(tail_span, tail_type, temp,
                                                   temp_name)))));
  return {};
}

auto drop_emitter::flag_ref(std::size_t flag, source_span span)
    -> outcome<ptr<hir_expr>> {
  if (flag >= flag_symbols_.size() ||
      flag_symbols_[flag] == k_invalid_symbol_id) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "internal error: a drop flag is used before the binding "
                "that declares it was lowered");
  }
  return ptr<hir_expr>(make<hir_local_ref>(span, checked_.types.bool_type(),
                                     flag_symbols_[flag],
                                     std::string("<drop flag>")));
}

auto drop_emitter::set_flags(const std::vector<std::size_t> &flags, bool value,
                        source_span span, ptr_vec<hir_node> &out)
    -> outcome<void> {
  for (const auto flag : flags) {
    auto target = flag_ref(flag, span);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    out.push_back(ptr<hir_node>(hir::make<hir_assign>(
        span, ast::assign_op::assign, std::move(*target),
        ptr<hir_expr>(make<hir_literal>(span, checked_.types.bool_type(),
                                        value ? token_kind::kw_true
                                              : token_kind::kw_false,
                                        value ? "true" : "false")))));
  }
  return {};
}

auto drop_emitter::declare_flags(const void *node, source_span span,
                            ptr_vec<hir_node> &out) -> void {
  const auto found = schedule_.binding_flags.find(node);
  if (found == schedule_.binding_flags.end()) {
    return;
  }
  for (const auto flag : found->second) {
    flag_symbols_[flag] = env_.mint();
    out.push_back(ptr<hir_node>(make<hir_let>(
        span, flag_symbols_[flag], std::string("<drop flag>"),
        ptr<hir_expr>(make<hir_literal>(span, checked_.types.bool_type(),
                                        token_kind::kw_true, "true")),
        /*mut=*/true)));
  }
}

auto drop_emitter::guarded(const std::vector<std::size_t> &flags,
                      ptr_vec<hir_node> body, source_span span)
    -> outcome<ptr_vec<hir_node>> {
  if (flags.empty() || body.empty()) {
    return body;
  }
  // One `if` per flag, the last innermost, rather than one `if` on their
  // `&&`: each test reads a flag itself, which is how the storage form's
  // verifier ties a value's drop to the flag that says it is still held.
  auto out = std::move(body);
  for (auto flag = flags.rbegin(); flag != flags.rend(); ++flag) {
    auto condition = flag_ref(*flag, span);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto branches = std::vector<hir_if_branch>{};
    branches.push_back(hir_if_branch{
        .condition = std::move(*condition),
        .body = make<hir_block>(span, k_unknown_type, std::move(out))});
    out = ptr_vec<hir_node>{};
    out.push_back(ptr<hir_node>(make<hir_expr_stmt>(
        span, ptr<hir_expr>(make<hir_if>(span, k_unknown_type,
                                         std::move(branches), nullptr)))));
  }
  return out;
}


} // namespace cinder::hir
