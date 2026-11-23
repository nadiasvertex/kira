#include <format>
#include <optional>
#include <string>
#include <utility>

#include "src/hir/lower_core_lowerer.h"

/// Statements and blocks.
namespace cinder::hir {

namespace {

/// The pattern `let` a statement or result is, if it is one.
[[nodiscard]] auto pattern_let(const core::node *node)
    -> const core::match_expr * {
  if (node != nullptr && node->kind == core::node_kind::expr_stmt) {
    node = static_cast<const core::expr_stmt *>(node)->value.get();
  }
  if (node == nullptr || node->kind != core::node_kind::match_expr) {
    return nullptr;
  }
  const auto *matched = static_cast<const core::match_expr *>(node);
  return matched->form == core::match_form::let_pattern ? matched : nullptr;
}

/// Whether `source` is the block a `for` loop elaborates to: its state,
/// then the loop (or, over an `option`, its one match).
[[nodiscard]] auto is_for_block(const core::block &source) -> bool {
  if (source.statements.empty() || source.result != nullptr) {
    return false;
  }
  const auto &last = *source.statements.back();
  if (last.kind == core::node_kind::while_loop) {
    return last.origin != nullptr;
  }
  if (last.kind != core::node_kind::expr_stmt) {
    return false;
  }
  const auto *value = static_cast<const core::expr_stmt &>(last).value.get();
  return value != nullptr && value->kind == core::node_kind::match_expr &&
         static_cast<const core::match_expr *>(value)->form ==
             core::match_form::for_option;
}

/// The loop match a `while_loop` holds alone: a `while let`, or a `for`
/// loop's test of `next`.
[[nodiscard]] auto loop_match(const core::while_loop &source)
    -> const core::match_expr * {
  if (source.condition != nullptr || source.step != nullptr ||
      source.body->result != nullptr || source.body->statements.size() != 1 ||
      source.body->statements[0]->kind != core::node_kind::expr_stmt) {
    return nullptr;
  }
  const auto *value =
      static_cast<const core::expr_stmt &>(*source.body->statements[0])
          .value.get();
  if (value == nullptr || value->kind != core::node_kind::match_expr) {
    return nullptr;
  }
  const auto *matched = static_cast<const core::match_expr *>(value);
  return matched->form == core::match_form::while_let ||
                 matched->form == core::match_form::for_next
             ? matched
             : nullptr;
}

} // namespace

/// The key the drop schedule names the local a core `let` declares by.
[[nodiscard]] auto binding_key(const core::let &source) -> const void * {
  return source.binding_origin;
}

auto core_lowerer::whole_local(const void *key) const
    -> std::optional<symbol_id> {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    for (auto bound = scope->rbegin(); bound != scope->rend(); ++bound) {
      if (bound->first == key) {
        return bound->second;
      }
    }
  }
  return std::nullopt;
}

auto core_lowerer::declare(symbol_id symbol, const std::string &name)
    -> symbol_id {
  const auto made = mint();
  symbols_.insert_or_assign(symbol, made);
  names_.insert_or_assign(made, name);
  return made;
}

auto core_lowerer::block(const core::block &source, type_id type)
    -> core_result<ptr<hir_block>> {
  push_scope();
  auto made = block_in_scope(source, type);
  pop_scope();
  return made;
}

auto core_lowerer::block_in_scope(const core::block &source, type_id type)
    -> core_result<ptr<hir_block>> {
  auto stmts = ptr_vec<hir_node>{};
  if (auto done = statements_into(source, type, stmts); !done) {
    return std::unexpected(done.error());
  }
  // The drops of this scope's bindings, unless it never ends normally. A
  // valued tail is held in a temporary across them.
  if (source.origin != nullptr &&
      (stmts.empty() || !always_exits(*stmts.back()))) {
    const auto value_typed =
        type != k_unknown_type && !checked_.types.is_unit(type);
    auto dropped =
        value_typed ? drops_.emit_tail_scope_exit_drops(source.origin, stmts)
                    : drops_.emit_scope_exit_drops(source.origin, stmts);
    if (!dropped.has_value()) {
      return refuse_drop(dropped.error());
    }
  }
  return hir::make<hir_block>(source.span, type, std::move(stmts));
}

auto core_lowerer::statements_into(const core::block &source, type_id type,
                                   ptr_vec<hir_node> &stmts)
    -> core_result<void> {
  // A pattern `let` ends the block: its statements, then the rest of the
  // block (its arm's body) in the same scope.
  const auto carry_on = [&](const core::match_expr &let,
                            const core::node &statement) -> core_result<void> {
    const auto first = stmts.size();
    auto hoisted = ptr_vec<hir_node>{};
    auto rest = hoisting(hoisted, [&] { return let_pattern_into(let, stmts); });
    if (!rest.has_value()) {
      return std::unexpected(rest.error());
    }
    place_hoisted(hoisted, stmts, first);
    for (auto i = first; i < stmts.size(); ++i) {
      stmts[i]->continues_statement = i > first;
    }
    if (first < stmts.size() && stmts[first]->origin == nullptr) {
      stmts[first]->origin = statement.origin;
    }
    // The rest of the function's body is still the function's body.
    if (&source == function_body_) {
      function_body_ = &**rest;
    }
    return statements_into(**rest, type, stmts);
  };
  // The HIR statements of one source statement: they end its temporaries
  // together (`make_references_explicit`), and the first names it.
  auto group = stmts.size();
  // The function body's trailing exit or postcondition checks wait for its
  // drops (`finish_contracts`); the exit's value stands as the tail.
  const auto count = tail_contracts(source);
  for (std::size_t i = 0; i < count; ++i) {
    const auto &statement = *source.statements[i];
    if (const auto *let = pattern_let(&statement)) {
      return carry_on(*let, statement);
    }
    const auto last = i + 1 == count && !source.result && !tail_exit_;
    const auto first = stmts.size();
    auto done = stmt_into(statement, stmts,
                          last ? std::optional{type} : std::nullopt);
    if (!done.has_value()) {
      return std::unexpected(done.error());
    }
    if (first == stmts.size()) {
      continue;
    }
    if (!statement.continues_statement || i == 0) {
      group = first;
    } else {
      stmts[first]->continues_statement = true;
    }
    if (stmts[group]->origin == nullptr && statement.origin != nullptr) {
      stmts[group]->origin = statement.origin;
    }
  }
  // A value as the block's last statement, after what it waits for.
  const auto tail_value = [&](const core::expr &result) -> core_result<void> {
    auto hoisted = ptr_vec<hir_node>{};
    auto value = hoisting(hoisted, [&] { return expr(result); });
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    const auto first = stmts.size();
    for (auto &node : hoisted) {
      stmts.push_back(std::move(node));
    }
    const auto span = (*value)->span;
    stmts.push_back(hir::make<hir_expr_stmt>(span, std::move(*value)));
    for (auto i = first + 1; i < stmts.size(); ++i) {
      stmts[i]->continues_statement = true;
    }
    return {};
  };
  if (count < source.statements.size() && tail_exit_ != nullptr) {
    const auto &bound = static_cast<const core::let &>(
        *tail_exit_->statements.front());
    if (auto done = tail_value(*bound.value); !done) {
      return done;
    }
  }
  if (source.result != nullptr) {
    if (const auto *let = pattern_let(source.result.get())) {
      return carry_on(*let, *source.result);
    }
    return tail_value(*source.result);
  }
  return {};
}

auto core_lowerer::for_block_into(const core::block &source,
                                  ptr_vec<hir_node> &out)
    -> core_result<void> {
  push_scope();
  auto done = [&]() -> core_result<void> {
    for (const auto &statement : source.statements) {
      if (auto made = stmt_into(*statement, out, std::nullopt); !made) {
        return made;
      }
    }
    // The iterator the loop holds drops after it.
    if (source.origin != nullptr) {
      auto dropped = drops_.emit_scope_exit_drops(source.origin, out);
      if (!dropped.has_value()) {
        return refuse_drop(dropped.error());
      }
    }
    return {};
  }();
  pop_scope();
  return done;
}

auto core_lowerer::if_expr(const core::if_expr &source, type_id type)
    -> core_result<ptr<hir_if>> {
  auto branches = std::vector<hir_if_branch>{};
  auto else_body = ptr<hir_block>{};
  auto dead_condition_type = std::optional<type_id>{};
  // An `elif` chain arrives as an `else` block holding the next `if`; the
  // old lowerer keeps the chain as the branches of one `hir_if`.
  for (const auto *current = &source; current != nullptr;) {
    // A later condition runs only when the earlier ones fail.
    const auto later = current != &source;
    conditional_depth_ += later ? 1 : 0;
    auto condition = expr(*current->condition);
    conditional_depth_ -= later ? 1 : 0;
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    // A condition that is the literal `false` (`needs_drop[T]()` for a
    // type with nothing to drop) never runs its body, which the old
    // lowerer does not lower at all.
    const auto dead =
        (*condition)->kind == hir_node_kind::hir_literal &&
        dynamic_cast<const hir_literal &>(**condition).lit_kind ==
            token_kind::kw_false;
    if (dead) {
      dead_condition_type = (*condition)->type;
    } else {
      auto body = block(*current->then_branch, type);
      if (!body.has_value()) {
        return std::unexpected(body.error());
      }
      branches.push_back(hir_if_branch{.condition = std::move(*condition),
                                       .body = std::move(*body)});
    }
    const auto *next = current->else_branch.get();
    current = nullptr;
    if (next == nullptr) {
      break;
    }
    const auto *chained = [&]() -> const core::if_expr * {
      if (next->origin != nullptr) {
        return nullptr;
      }
      if (next->result != nullptr && next->statements.empty() &&
          next->result->kind == core::node_kind::if_expr) {
        return static_cast<const core::if_expr *>(next->result.get());
      }
      if (next->result == nullptr && next->statements.size() == 1 &&
          next->statements.front()->kind == core::node_kind::expr_stmt) {
        const auto &only =
            static_cast<const core::expr_stmt &>(*next->statements.front());
        if (only.value != nullptr &&
            only.value->kind == core::node_kind::if_expr) {
          return static_cast<const core::if_expr *>(only.value.get());
        }
      }
      return nullptr;
    }();
    if (chained != nullptr) {
      current = chained;
      continue;
    }
    auto fallback = block(*next, type);
    if (!fallback.has_value()) {
      return std::unexpected(fallback.error());
    }
    else_body = std::move(*fallback);
  }
  if (branches.empty()) {
    // Every branch was dead: the fallback stays an `hir_if` guarded by a
    // constant `true`, as the old lowerer leaves it.
    auto fallback = else_body != nullptr
                        ? std::move(else_body)
                        : hir::make<hir_block>(source.span, type,
                                               ptr_vec<hir_node>{});
    branches.push_back(hir_if_branch{
        .condition = hir::make<hir_literal>(
            source.span, dead_condition_type.value_or(k_unknown_type),
            token_kind::kw_true, "true"),
        .body = std::move(fallback)});
  }
  return hir::make<hir_if>(source.span, type, std::move(branches),
                      std::move(else_body));
}

auto core_lowerer::stmt_into(const core::stmt &source, ptr_vec<hir_node> &out,
                             std::optional<type_id> tail_type)
    -> core_result<void> {
  const auto first = out.size();
  // In a task, what the statement waits for goes ahead of it.
  auto hoisted = ptr_vec<hir_node>{};
  auto done = hoisting(hoisted, [&]() -> core_result<void> {
    using k = core::node_kind;
    switch (source.kind) {
    case k::let:
      return let_into(static_cast<const core::let &>(source), out);
    case k::assign:
      return assign_into(static_cast<const core::assign &>(source), out);
    case k::expr_stmt: {
      const auto &statement = static_cast<const core::expr_stmt &>(source);
      // A `return` through postconditions or crew ends, flat as the old
      // lowerer makes it.
      if (const auto *exit = exit_block(&source)) {
        return exit_into(*exit, out);
      }
      // A field write's invariant check: `let self = &object; <check>`,
      // flat after the write.
      if (statement.value != nullptr) {
        if (const auto *checked = invariant_block(*statement.value)) {
          for (const auto &inner : checked->statements) {
            if (auto made = stmt_into(*inner, out, std::nullopt); !made) {
              return made;
            }
          }
          return {};
        }
      }
      return expr_stmt_into(statement, out, tail_type);
    }
    case k::contract_check:
      return contract_into(static_cast<const core::contract_check &>(source),
                           out);
    case k::while_loop:
      return while_into(static_cast<const core::while_loop &>(source), out);
    case k::return_stmt:
    case k::yield_stmt:
    case k::break_stmt:
    case k::continue_stmt:
      return jump_into(static_cast<const core::jump &>(source), out);
    default:
      return refuse(source);
    }
  });
  if (!done.has_value()) {
    return done;
  }
  place_hoisted(hoisted, out, first);
  // A `for` loop's element binding and the body spliced after it are
  // statements of their own.
  const auto spliced_body =
      source.kind == core::node_kind::expr_stmt &&
      static_cast<const core::expr_stmt &>(source).value != nullptr &&
      static_cast<const core::expr_stmt &>(source).value->kind ==
          core::node_kind::match_expr &&
      static_cast<const core::match_expr &>(
          *static_cast<const core::expr_stmt &>(source).value)
              .form == core::match_form::for_element;
  if (spliced_body) {
    if (first < out.size()) {
      out[first]->continues_statement = false;
    }
    return {};
  }
  for (auto i = first; i < out.size(); ++i) {
    out[i]->continues_statement = i > first;
  }
  return {};
}

auto core_lowerer::let_into(const core::let &source, ptr_vec<hir_node> &out)
    -> core_result<void> {
  if (source.value == nullptr) {
    return refuse("let with no value", source.span);
  }
  if (source.name == "<for iterator>") {
    for_adapter_ = source.value.get();
  }
  auto value = expr(*source.value);
  for_adapter_ = nullptr;
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto symbol = declare(source.symbol, source.name);
  // A `for` loop's adapter (`into_iter`, `iter`) hands the collection to
  // the iterator, which the old lowerer marks as consuming its receiver.
  if (source.binding_origin != nullptr && source.name == "<for iterator>" &&
      source.value->kind == core::node_kind::call &&
      source.value->origin == nullptr &&
      (*value)->kind == hir_node_kind::hir_call) {
    auto &adapter = dynamic_cast<hir_call &>(**value);
    const auto &target = static_cast<const core::call &>(*source.value).target;
    adapter.consumes_receiver = true;
    adapter.receiver_by_value =
        target.receiver == core::receiver_mode::consuming;
  }
  out.push_back(hir::make<hir_let>(source.span, symbol, source.name,
                              std::move(*value), source.is_mut));
  if (const auto *key = binding_key(source)) {
    binding_keys_.insert_or_assign(source.symbol, key);
    own(key, symbol);
    drops_.declare_flags(key, source.span, out);
  }
  return {};
}

auto core_lowerer::assign_into(const core::assign &source,
                               ptr_vec<hir_node> &out) -> core_result<void> {
  auto target = place(source.target, source.span);
  if (!target.has_value()) {
    return std::unexpected(target.error());
  }
  auto value = expr(*source.value);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  // `a op= b`, which the core spells `a = a op b`, neither drops the old
  // value nor sets flags, as in the old lowerer.
  if (source.compound) {
    out.push_back(hir::make<hir_assign>(source.span, ast::assign_op::assign,
                                        std::move(*target), std::move(*value)));
    return {};
  }
  // The old value drops after the new one is computed, and the target's
  // drop flags are set again after the write.
  auto assigned =
      drops_.assignment(source.origin, ast::assign_op::assign,
                        std::move(*target), std::move(*value), source.span, out);
  if (!assigned.has_value()) {
    return refuse_drop(assigned.error());
  }
  return {};
}

auto core_lowerer::expr_stmt_into(const core::expr_stmt &source,
                                  ptr_vec<hir_node> &out,
                                  std::optional<type_id> tail_type)
    -> core_result<void> {
  const auto &value = *source.value;
  // A crew ends before a jump that leaves it.
  if (value.kind == core::node_kind::call &&
      static_cast<const core::call &>(value).target.kind ==
          core::callee::form::intrinsic &&
      static_cast<const core::call &>(value).target.name == "crew_end") {
    return crew_end_call_into(static_cast<const core::call &>(value), out);
  }
  // A `crew` statement: its statements stand among the enclosing ones.
  if (const auto *crew = crew_block(value)) {
    auto result = crew_into(*crew, hir_type(value.type), out);
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    if (*result != nullptr) {
      out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(*result)));
    }
    return {};
  }
  // An `if` statement: in a block's tail it is the block's value, typed as
  // the block; elsewhere it stands alone, untyped.
  if (value.kind == core::node_kind::if_expr &&
      checked_.types.is_unit(value.type)) {
    const auto &chain = static_cast<const core::if_expr &>(value);
    auto made = if_expr(chain, tail_type.value_or(k_unknown_type));
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    (*made)->origin = value.origin;
    if (tail_type.has_value()) {
      out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(*made)));
    } else {
      out.push_back(std::move(*made));
    }
    return {};
  }
  if (value.kind == core::node_kind::match_expr) {
    const auto &matched = static_cast<const core::match_expr &>(value);
    if (matched.form == core::match_form::for_element) {
      push_scope();
      auto done = for_element_into(matched, out);
      pop_scope();
      return done;
    }
    if (checked_.types.is_unit(value.type) &&
        matched.form != core::match_form::let_pattern) {
      auto made = match(matched, tail_type.value_or(k_unknown_type));
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      if ((*made)->origin == nullptr) {
        (*made)->origin = value.origin;
      }
      // A `match` or `if let` statement stands alone, as an `if` does,
      // unless it is the block's tail.
      const auto statement = matched.form == core::match_form::match ||
                             matched.form == core::match_form::if_let;
      if (statement && !tail_type.has_value()) {
        out.push_back(std::move(*made));
      } else {
        out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(*made)));
      }
      return {};
    }
  }
  if (value.kind == core::node_kind::block &&
      checked_.types.is_unit(value.type)) {
    const auto *loop = static_cast<const core::block *>(&value);
    // A comprehension clause wraps its loop in a block of its own.
    if (loop->origin == nullptr && loop->result == nullptr &&
        loop->statements.size() == 1 &&
        loop->statements[0]->kind == core::node_kind::expr_stmt) {
      const auto *inner =
          static_cast<const core::expr_stmt &>(*loop->statements[0])
              .value.get();
      if (inner != nullptr && inner->kind == core::node_kind::block &&
          is_for_block(static_cast<const core::block &>(*inner))) {
        loop = static_cast<const core::block *>(inner);
      }
    }
    if (is_for_block(*loop)) {
      return for_block_into(*loop, out);
    }
  }
  if (value.kind == core::node_kind::block &&
      checked_.types.is_unit(value.type)) {
    auto made = block(static_cast<const core::block &>(value),
                      hir_type_in_statement(value.type));
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(*made)));
    return {};
  }
  auto lowered = expr(value);
  if (!lowered.has_value()) {
    return std::unexpected(lowered.error());
  }
  out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(*lowered)));
  return {};
}

auto core_lowerer::while_into(const core::while_loop &source,
                              ptr_vec<hir_node> &out) -> core_result<void> {
  if (const auto *matched = loop_match(source)) {
    return while_let_into(*matched, out);
  }
  auto condition = ptr<hir_expr>{};
  // In a task, what the condition waits for runs at the top of every
  // iteration; elsewhere the condition is evaluated repeatedly.
  auto hoisted = ptr_vec<hir_node>{};
  if (source.condition != nullptr) {
    auto lowered = [&] {
      if (in_task_body_ && hoist_ != nullptr) {
        auto *const enclosing = std::exchange(hoist_, &hoisted);
        auto made = expr(*source.condition);
        hoist_ = enclosing;
        return made;
      }
      ++conditional_depth_;
      auto made = expr(*source.condition);
      --conditional_depth_;
      return made;
    }();
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    condition = std::move(*lowered);
  } else {
    condition = hir::make<hir_literal>(source.span, checked_.types.bool_type(),
                                  token_kind::kw_true, "true");
  }
  auto body = block(*source.body, k_unknown_type);
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  auto step = ptr<hir_block>{};
  // A `for` loop's step `i = i + n` is `i += n`, as the old lowerer
  // spells it.
  const auto *advance =
      source.step != nullptr &&
              source.step->statements.size() == 1 &&
              source.step->statements[0]->kind == core::node_kind::assign
          ? static_cast<const core::assign *>(source.step->statements[0].get())
          : nullptr;
  const auto *sum = advance != nullptr && advance->value != nullptr &&
                            advance->value->kind == core::node_kind::primitive
                        ? static_cast<const core::primitive *>(
                              advance->value.get())
                        : nullptr;
  if (sum != nullptr && sum->binary == ast::binary_op::add &&
      sum->operands.size() == 2 &&
      sum->operands[0]->kind == core::node_kind::use &&
      advance->target.path.empty() &&
      static_cast<const core::use &>(*sum->operands[0]).from.root.symbol ==
          advance->target.root.symbol) {
    auto target = place(advance->target, advance->span);
    if (!target.has_value()) {
      return std::unexpected(target.error());
    }
    auto by = expr(*sum->operands[1]);
    if (!by.has_value()) {
      return std::unexpected(by.error());
    }
    auto stmts = ptr_vec<hir_node>{};
    stmts.push_back(hir::make<hir_assign>(advance->span,
                                          ast::assign_op::add_assign,
                                          std::move(*target), std::move(*by)));
    step = hir::make<hir_block>(source.span, k_unknown_type, std::move(stmts));
  } else if (source.step != nullptr) {
    auto lowered = block(*source.step, k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    step = std::move(*lowered);
  }
  if (!hoisted.empty()) {
    // The condition waits for a task, and a wait is a statement, so the
    // test moves into the loop: `while true: <wait>; if not cond: break;
    // body`.
    auto exit = ptr_vec<hir_node>{};
    exit.push_back(hir::make<hir_break>(source.span));
    auto branches = std::vector<hir_if_branch>{};
    branches.push_back(hir_if_branch{
        .condition = hir::make<hir_unary>(source.span,
                                          checked_.types.bool_type(),
                                          ast::unary_op::logical_not,
                                          std::move(condition)),
        .body = hir::make<hir_block>(source.span, k_unknown_type,
                                     std::move(exit))});
    hoisted.push_back(hir::make<hir_expr_stmt>(
        source.span, hir::make<hir_if>(source.span, k_unknown_type,
                                       std::move(branches), nullptr)));
    for (auto &stmt : (*body)->stmts) {
      hoisted.push_back(std::move(stmt));
    }
    (*body)->stmts = std::move(hoisted);
    condition = hir::make<hir_literal>(source.span, checked_.types.bool_type(),
                                       token_kind::kw_true, "true");
  }
  out.push_back(hir::make<hir_while>(source.span, std::move(condition),
                                std::move(*body), std::move(step)));
  return {};
}

auto core_lowerer::jump_into(const core::jump &source, ptr_vec<hir_node> &out)
    -> core_result<void> {
  // Before a jump: the placeholder `make_references_explicit` fills with the
  // drops of the temporaries the jump leaves alive, then the drops of the
  // scopes it leaves. A `return` of a value with nothing scheduled drops its
  // temporaries as part of its own full expression.
  const auto exit_drops = [&](jump_exit exit) -> core_result<void> {
    if (source.origin == nullptr) {
      auto made = hir::make<hir_block>(source.span, k_unknown_type,
                                       ptr_vec<hir_node>{});
      made->exit = exit;
      out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(made)));
      return {};
    }
    auto dropped = drops_.emit_jump_drops(source.origin, exit, source.span, out);
    if (!dropped.has_value()) {
      return refuse_drop(dropped.error());
    }
    return {};
  };
  switch (source.kind) {
  case core::node_kind::return_stmt: {
    if (source.value == nullptr) {
      if (auto dropped = exit_drops(jump_exit::function); !dropped) {
        return dropped;
      }
      out.push_back(hir::make<hir_return>(source.span, nullptr));
      return {};
    }
    auto value = expr(*source.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    // With drops scheduled, the value is read into a temporary first: the
    // drops run after the read, and the return yields the temporary.
    const auto scheduled = drops_.schedule().exits.find(source.origin);
    if (source.origin != nullptr &&
        scheduled != drops_.schedule().exits.end() &&
        !scheduled->second.empty() && (*value)->type != k_unknown_type) {
      const auto value_type = (*value)->type;
      const auto temp = mint();
      const auto temp_name = std::format("<return value {}>", temp);
      out.push_back(hir::make<hir_let>(source.span, temp, temp_name,
                                       std::move(*value)));
      if (auto dropped = exit_drops(jump_exit::function); !dropped) {
        return dropped;
      }
      value = ptr<hir_expr>(hir::make<hir_local_ref>(
          source_span::dummy(), value_type, temp, temp_name));
    }
    // A task hands its result over with a `yield`, then finishes.
    if (in_task_body_) {
      out.push_back(hir::make<hir_yield>(source.span, std::move(*value)));
      out.push_back(hir::make<hir_return>(source.span, nullptr));
      return {};
    }
    out.push_back(hir::make<hir_return>(source.span, std::move(*value)));
    return {};
  }
  case core::node_kind::yield_stmt: {
    if (source.value == nullptr) {
      return refuse_feature("yield with no value", "`yield` with no value is not supported", source.span);
    }
    auto value = expr(*source.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    out.push_back(hir::make<hir_yield>(source.span, std::move(*value)));
    // Resumed only to be dropped: return from here.
    auto cancel = cancel_check(source.suspension, source.span);
    if (!cancel.has_value()) {
      return std::unexpected(cancel.error());
    }
    out.push_back(std::move(*cancel));
    return {};
  }
  case core::node_kind::break_stmt:
    if (auto dropped = exit_drops(jump_exit::loop); !dropped) {
      return dropped;
    }
    out.push_back(hir::make<hir_break>(source.span));
    return {};
  case core::node_kind::continue_stmt:
    if (auto dropped = exit_drops(jump_exit::loop); !dropped) {
      return dropped;
    }
    out.push_back(hir::make<hir_continue>(source.span));
    return {};
  default:
    return refuse(source);
  }
}

} // namespace cinder::hir
