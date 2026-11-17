#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "src/hir/captures.h"
#include "src/hir/lower_core_lowerer.h"
#include "src/hir/reference_check.h"
#include "src/runtime/scheduler.h"
#include "src/semantic/ownership_cfg.h"

/// Generators and tasks: suspension points, what a cancelled coroutine
/// runs, task blocks and crews.
///
/// In HIR a suspension is a statement. In a task body, what waits for a
/// task inside a statement goes ahead of it (`hoist_`), and an `if` or
/// `match` value whose branches may suspend becomes statements that assign
/// a `<branch value>` local.
namespace cinder::hir {

using semantic::type_kind;

namespace {

/// Replaces the value at the end of `block` with `slot = value`, following
/// a trailing `if`/`match`/block into its branches. A branch that ends in a
/// jump, or in a statement, has no value to assign. The same rewrite as the
/// old lowerer's `assign_branch_values` (`lower.cpp`).
auto assign_branch_values(hir_block &block, symbol_id slot, type_id type)
    -> void {
  if (block.stmts.empty()) {
    return;
  }
  auto &last = block.stmts.back();
  if (last->kind != hir_node_kind::hir_expr_stmt) {
    return;
  }
  auto &stmt = dynamic_cast<hir_expr_stmt &>(*last);
  auto &value = *stmt.expr;
  block.type = k_unknown_type;
  switch (value.kind) {
  case hir_node_kind::hir_if: {
    auto &branching = dynamic_cast<hir_if &>(value);
    for (auto &branch : branching.branches) {
      assign_branch_values(*branch.body, slot, type);
    }
    if (branching.else_body != nullptr) {
      assign_branch_values(*branching.else_body, slot, type);
    }
    branching.type = k_unknown_type;
    return;
  }
  case hir_node_kind::hir_match: {
    auto &branching = dynamic_cast<hir_match &>(value);
    for (auto &arm : branching.arms) {
      assign_branch_values(*arm.body, slot, type);
    }
    branching.type = k_unknown_type;
    return;
  }
  case hir_node_kind::hir_block:
    assign_branch_values(dynamic_cast<hir_block &>(value), slot, type);
    return;
  default:
    break;
  }
  if (value.type != type) {
    return;
  }
  last = hir::make<hir_assign>(
      stmt.span, ast::assign_op::assign,
      hir::make<hir_local_ref>(stmt.span, type, slot, "<branch value>"),
      std::move(stmt.expr));
}

[[nodiscard]] auto intrinsic_named(const core::node &node,
                                   std::string_view name) -> bool {
  if (node.kind != core::node_kind::call) {
    return false;
  }
  const auto &target = static_cast<const core::call &>(node).target;
  return target.kind == core::callee::form::intrinsic && target.name == name;
}

/// Whether evaluating `node` may suspend the task or leave it from inside
/// a branch: an `await`, `par` or `race`, a crew, a `?`, or a `return`,
/// `break` or `continue` the source wrote. A lambda or task block is a body
/// of its own, which suspends its own task.
[[nodiscard]] auto may_suspend(const core::node &node) -> bool;

[[nodiscard]] auto place_may_suspend(const core::place &at) -> bool {
  if (at.root.value != nullptr && may_suspend(*at.root.value)) {
    return true;
  }
  for (const auto &step : at.path) {
    if (step.subscript != nullptr && may_suspend(*step.subscript)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] auto any_may_suspend(const ptr_vec<core::expr> &all) -> bool {
  for (const auto &one : all) {
    if (one != nullptr && may_suspend(*one)) {
      return true;
    }
  }
  return false;
}

auto may_suspend(const core::node &node) -> bool {
  using k = core::node_kind;
  switch (node.kind) {
  case k::await_expr:
    return true;
  case k::return_stmt:
  case k::break_stmt:
  case k::continue_stmt:
    // A jump the elaboration made up (a loop's exit) is no source jump.
    return node.origin != nullptr;
  case k::lambda:
    return false;
  case k::literal:
  case k::function_ref:
  case k::yield_stmt:
    return false;
  case k::host_call:
    return any_may_suspend(static_cast<const core::host_call &>(node).args);
  case k::use:
    return place_may_suspend(static_cast<const core::use &>(node).from);
  case k::borrow:
    return place_may_suspend(static_cast<const core::borrow &>(node).of);
  case k::call: {
    const auto &made = static_cast<const core::call &>(node);
    if (made.target.kind == core::callee::form::intrinsic) {
      const auto &name = made.target.name;
      if (name == "await_par" || name == "await_race" || name == "crew_new" ||
          name == "task_yield") {
        return true;
      }
      // `on(...)` not awaited makes a task of its own.
      if (name == "on_cpu" || name == "on_io" || name == "task_block") {
        return false;
      }
    }
    if (made.target.value != nullptr && may_suspend(*made.target.value)) {
      return true;
    }
    return any_may_suspend(made.args);
  }
  case k::primitive:
    return any_may_suspend(static_cast<const core::primitive &>(node).operands);
  case k::cast:
    return may_suspend(*static_cast<const core::cast &>(node).operand);
  case k::tuple_init:
  case k::array_init:
  case k::struct_init:
    return any_may_suspend(
        static_cast<const core::aggregate_init &>(node).elements);
  case k::variant_init:
    return any_may_suspend(static_cast<const core::variant_init &>(node).payload);
  case k::block: {
    const auto &inner = static_cast<const core::block &>(node);
    for (const auto &statement : inner.statements) {
      if (may_suspend(*statement)) {
        return true;
      }
    }
    return inner.result != nullptr && may_suspend(*inner.result);
  }
  case k::if_expr: {
    const auto &branching = static_cast<const core::if_expr &>(node);
    return may_suspend(*branching.condition) ||
           may_suspend(*branching.then_branch) ||
           (branching.else_branch != nullptr &&
            may_suspend(*branching.else_branch));
  }
  case k::match_expr: {
    const auto &matched = static_cast<const core::match_expr &>(node);
    if (matched.form == core::match_form::try_operator) {
      return true;
    }
    if (place_may_suspend(matched.subject)) {
      return true;
    }
    for (const auto &arm : matched.arms) {
      if ((arm.guard != nullptr && may_suspend(*arm.guard)) ||
          (arm.body != nullptr && may_suspend(*arm.body))) {
        return true;
      }
    }
    return false;
  }
  case k::let: {
    const auto &bound = static_cast<const core::let &>(node);
    return bound.value != nullptr && may_suspend(*bound.value);
  }
  case k::assign: {
    const auto &assigned = static_cast<const core::assign &>(node);
    return place_may_suspend(assigned.target) ||
           (assigned.value != nullptr && may_suspend(*assigned.value));
  }
  case k::expr_stmt: {
    const auto &statement = static_cast<const core::expr_stmt &>(node);
    return statement.value != nullptr && may_suspend(*statement.value);
  }
  case k::while_loop: {
    const auto &loop = static_cast<const core::while_loop &>(node);
    return (loop.condition != nullptr && may_suspend(*loop.condition)) ||
           may_suspend(*loop.body) ||
           (loop.step != nullptr && may_suspend(*loop.step));
  }
  case k::contract_check: {
    const auto &check = static_cast<const core::contract_check &>(node);
    return check.condition != nullptr && may_suspend(*check.condition);
  }
  }
  return false;
}

/// The value a task of type `task_type` hands over: `T`, or `result[T, E]`
/// for a fallible task.
[[nodiscard]] auto task_result_of(semantic::type_table &types,
                                  type_id task_type) -> std::optional<type_id> {
  const auto &task = types.entry(task_type);
  if (task.kind != type_kind::builtin_generic_kind || task.name != "task" ||
      task.args.size() < 2) {
    return std::nullopt;
  }
  const auto fallible = types.entry(task.args[1]).name != "never";
  return fallible ? types.builtin_generic("result", {task.args[0], task.args[1]})
                  : task.args[0];
}

[[nodiscard]] auto is_task_type(const semantic::type_table &types, type_id id)
    -> bool {
  const auto &entry = types.entry(id);
  return entry.kind == type_kind::builtin_generic_kind && entry.name == "task";
}

} // namespace

auto core_lowerer::cancel_check(const void *key, source_span span,
                                ptr_vec<hir_expr> awaited,
                                ptr_vec<hir_node> drops)
    -> core_result<ptr<hir_node>> {
  auto exit = ptr_vec<hir_node>{};
  for (auto &task : awaited) {
    exit.push_back(hir::make<hir_expr_stmt>(
        span, hir::make<hir_coroutine_cancel>(span, k_unknown_type,
                                              std::move(task))));
  }
  for (auto &drop : drops) {
    exit.push_back(std::move(drop));
  }
  // Returning leaves every open crew, which ends first, innermost first.
  if (!open_crews_.empty() &&
      (value_branch_depth_ > 0 || conditional_depth_ > 0)) {
    return refuse_feature("crew exit where it cannot be a statement", k_crew_exit_message, span);
  }
  for (auto crew = open_crews_.rbegin(); crew != open_crews_.rend(); ++crew) {
    if (auto ended = crew_end_into(*crew, span, exit); !ended) {
      return std::unexpected(ended.error());
    }
  }
  if (auto dropped = drops_.emit_jump_drops(key, jump_exit::function, span,
                                            exit);
      !dropped.has_value()) {
    return refuse_drop(dropped.error());
  }
  exit.push_back(hir::make<hir_return>(span, nullptr));
  auto branches = std::vector<hir_if_branch>{};
  branches.push_back(hir_if_branch{
      .condition = hir::make<hir_coroutine_cancelled>(
          span, checked_.types.bool_type()),
      .body = hir::make<hir_block>(span, k_unknown_type, std::move(exit))});
  return ptr<hir_node>(hir::make<hir_expr_stmt>(
      span, hir::make<hir_if>(span, k_unknown_type, std::move(branches),
                              nullptr)));
}

auto core_lowerer::place_hoisted(ptr_vec<hir_node> &hoisted,
                                 ptr_vec<hir_node> &out, std::size_t at)
    -> void {
  if (hoisted.empty()) {
    return;
  }
  auto rest = ptr_vec<hir_node>{};
  for (auto i = at; i < out.size(); ++i) {
    rest.push_back(std::move(out[i]));
  }
  out.resize(at);
  for (auto &node : hoisted) {
    out.push_back(std::move(node));
  }
  for (auto &node : rest) {
    out.push_back(std::move(node));
  }
  hoisted.clear();
}

auto core_lowerer::branch_value_suspends(const core::expr &source,
                                         type_id type) const -> bool {
  if (!in_task_body_ || cannot_hoist() || type == k_unknown_type ||
      checked_.types.is_unit(type)) {
    return false;
  }
  return may_suspend(source);
}

auto core_lowerer::suspending_branch_value(
    source_span span, type_id type,
    const std::function<core_result<ptr<hir_expr>>()> &lower)
    -> core_result<ptr<hir_expr>> {
  // The condition or subject waits into the enclosing statement's prelude
  // as usual; the branches are statement lists with preludes of their own.
  auto *prelude = hoist_;
  auto lowered = lower();
  if (!lowered.has_value()) {
    return lowered;
  }
  const auto slot = mint();
  names_.insert_or_assign(slot, "<branch value>");
  auto holder = ptr_vec<hir_node>{};
  holder.push_back(hir::make<hir_expr_stmt>(span, std::move(*lowered)));
  auto wrapper = hir_block(span, type, std::move(holder));
  assign_branch_values(wrapper, slot, type);
  auto declared = hir::make<hir_let>(span, slot, "<branch value>", nullptr,
                                     /*mut=*/true);
  declared->declared_type = type;
  prelude->push_back(std::move(declared));
  for (auto &stmt : wrapper.stmts) {
    prelude->push_back(std::move(stmt));
  }
  return ptr<hir_expr>(
      hir::make<hir_local_ref>(span, type, slot, "<branch value>"));
}

auto core_lowerer::runtime_call(source_span span, std::string_view name,
                                type_id result, ptr_vec<hir_expr> args)
    -> ptr<hir_expr> {
  auto callee = hir::make<hir_local_ref>(span, k_unknown_type,
                                         callee_symbol(name), std::string(name));
  return hir::make<hir_call>(span, result, std::move(callee), std::move(args));
}

namespace {

[[nodiscard]] auto pool_literal(const semantic::type_table &types,
                                source_span span,
                                runtime::scheduler::pool where)
    -> ptr<hir_expr> {
  return hir::make<hir_literal>(
      span, types.usize_type(), token_kind::int_lit,
      std::to_string(static_cast<std::uint64_t>(where)));
}

} // namespace

auto core_lowerer::task_wait_into(ptr_vec<hir_node> &stmts, ptr<hir_expr> task,
                                  const void *key, source_span span, bool all,
                                  ptr_vec<hir_expr> awaited)
    -> core_result<void> {
  stmts.push_back(hir::make<hir_yield>(span, std::move(task),
                                       /*awaiting_task=*/true,
                                       /*awaiting_all=*/all));
  auto cancel = cancel_check(key, span, std::move(awaited));
  if (!cancel.has_value()) {
    return std::unexpected(cancel.error());
  }
  stmts.push_back(std::move(*cancel));
  return {};
}

auto core_lowerer::read_then_drop_tasks(ptr_vec<hir_node> &stmts,
                                        ptr<hir_expr> read,
                                        ptr_vec<hir_expr> tasks, type_id type,
                                        source_span span) -> ptr<hir_expr> {
  const auto result = mint();
  names_.insert_or_assign(result, "<await result>");
  stmts.push_back(
      hir::make<hir_let>(span, result, "<await result>", std::move(read)));
  for (auto &task : tasks) {
    stmts.push_back(hir::make<hir_expr_stmt>(
        span, hir::make<hir_coroutine_cancel>(span, k_unknown_type,
                                              std::move(task))));
  }
  return hir::make<hir_local_ref>(span, type, result, "<await result>");
}

auto core_lowerer::task_read(const void *key, source_span span,
                             source_span task_span, type_id type,
                             type_id task_type, ptr<hir_expr> task,
                             bool consumes) -> core_result<ptr<hir_expr>> {
  if (!in_task_body_) {
    // An ordinary function (the entry `async def main`) has no state
    // machine to suspend: it blocks the calling thread until the task is
    // done.
    if (!consumes) {
      return ptr<hir_expr>(hir::make<hir_generator_next>(
          span, type, std::move(task), /*await_task=*/true));
    }
    const auto held = mint();
    names_.insert_or_assign(held, "<await task>");
    const auto held_ref = [&]() -> ptr<hir_expr> {
      return hir::make<hir_local_ref>(task_span, task_type, held,
                                      "<await task>");
    };
    auto stmts = ptr_vec<hir_node>{};
    stmts.push_back(
        hir::make<hir_let>(task_span, held, "<await task>", std::move(task)));
    auto drops = ptr_vec<hir_expr>{};
    drops.push_back(held_ref());
    auto result = read_then_drop_tasks(
        stmts,
        hir::make<hir_generator_next>(span, type, held_ref(),
                                      /*await_task=*/true),
        std::move(drops), type, span);
    stmts.push_back(hir::make<hir_expr_stmt>(span, std::move(result)));
    return ptr<hir_expr>(hir::make<hir_block>(span, type, std::move(stmts)));
  }
  if (cannot_hoist()) {
    return refuse_feature("await where it cannot go ahead of its statement", k_conditional_await_message, span);
  }
  // `await e` waits for `e` in statements ahead of the one that uses its
  // result. `t.get()` on a local waits on `t` where it stands.
  const auto *named = !consumes && task->kind == hir_node_kind::hir_local_ref
                          ? &static_cast<const hir_local_ref &>(*task)
                          : nullptr;
  const auto task_symbol = named != nullptr ? named->symbol : mint();
  const auto task_name =
      named != nullptr ? named->name : std::string("<await task>");
  const auto task_ref = [&, task_symbol]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(task_span, task_type, task_symbol,
                                    task_name);
  };
  if (named == nullptr) {
    names_.insert_or_assign(task_symbol, task_name);
    hoist_->push_back(
        hir::make<hir_let>(task_span, task_symbol, task_name, std::move(task)));
  }
  auto held = ptr_vec<hir_expr>{};
  if (consumes) {
    held.push_back(task_ref());
  }
  if (auto waited = task_wait_into(*hoist_, task_ref(), key, span,
                                   /*all=*/false, std::move(held));
      !waited.has_value()) {
    return std::unexpected(waited.error());
  }
  auto read = ptr<hir_expr>(hir::make<hir_generator_next>(
      span, type, task_ref(), /*await_task=*/true));
  if (!consumes) {
    return read;
  }
  auto drops = ptr_vec<hir_expr>{};
  drops.push_back(task_ref());
  return read_then_drop_tasks(*hoist_, std::move(read), std::move(drops), type,
                              span);
}

auto core_lowerer::try_in_task(const core::match_expr &source, type_id type)
    -> core_result<ptr<hir_expr>> {
  if (cannot_hoist()) {
    return refuse_feature("? where it cannot go ahead of its statement", k_crew_exit_message, source.span);
  }
  auto made = match(source, type);
  if (!made.has_value()) {
    return std::unexpected(made.error());
  }
  auto &tested = **made;
  if (tested.arms.size() != 2 ||
      tested.arms[0].pattern->kind != hir_node_kind::hir_constructor_pattern) {
    return refuse("? with no success arm", source.span);
  }
  const auto success =
      static_cast<const hir_constructor_pattern &>(*tested.arms[0].pattern)
          .variant_name;
  const auto subject = tested.subject_symbol;
  const auto subject_type = tested.subject->type;
  const auto subject_span = tested.subject->span;
  const auto name = names_.contains(subject) ? names_.at(subject)
                                             : std::string("<try subject>");
  const auto read = [&]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(subject_span, subject_type, subject, name);
  };
  hoist_->push_back(hir::make<hir_let>(subject_span, subject, name,
                                       std::move(tested.subject)));
  auto arms = std::vector<hir_match_arm>{};
  arms.push_back(std::move(tested.arms[1]));
  arms.push_back(hir_match_arm{
      .pattern = hir::make<hir_wildcard_pattern>(source.span),
      .guard = nullptr,
      .body = hir::make<hir_block>(source.span, k_unknown_type,
                                   ptr_vec<hir_node>{})});
  hoist_->push_back(hir::make<hir_match>(source.span, k_unknown_type, read(),
                                         mint(), std::move(arms)));
  return ptr<hir_expr>(
      hir::make<hir_variant_payload>(source.span, type, read(), success, 0));
}

auto core_lowerer::await_value(const core::await_expr &source)
    -> core_result<ptr<hir_expr>> {
  const auto type = hir_type(source.type);
  const auto &task = *source.task;
  // `await on(ctx, ...)`: the task runs on the context's pool.
  if (intrinsic_named(task, "on_cpu") || intrinsic_named(task, "on_io")) {
    const auto &on = static_cast<const core::call &>(task);
    if (on.args.size() != 1) {
      return refuse("on with no task", on.span);
    }
    const auto where = on.target.name == "on_io"
                           ? runtime::scheduler::pool::io
                           : runtime::scheduler::pool::cpu;
    if (in_task_body_ && cannot_hoist()) {
      return refuse_feature("await where it cannot go ahead of its statement", k_conditional_await_message,
                    source.span);
    }
    const auto &sender = *on.args.front();
    auto made = intrinsic_named(sender, "task_block")
                    ? task_block(static_cast<const core::call &>(sender))
                    : expr(sender);
    if (!made.has_value()) {
      return made;
    }
    return spawn_and_wait(std::move(*made), hir_type(sender.type),
                          static_cast<std::size_t>(where), type, source.origin,
                          source.span);
  }
  auto lowered = expr(task);
  if (!lowered.has_value()) {
    return lowered;
  }
  return task_read(source.origin, source.span, task.span, type,
                   hir_type(task.type), std::move(*lowered), /*consumes=*/true);
}

auto core_lowerer::spawn_and_wait(ptr<hir_expr> made, type_id task_type,
                                  std::size_t pool, type_id type,
                                  const void *wait_key, source_span span)
    -> core_result<ptr<hir_expr>> {
  const auto in_task = in_task_body_;
  auto local_setup = ptr_vec<hir_node>{};
  if (in_task && hoist_ == nullptr) {
    return refuse_feature("await where it cannot go ahead of its statement", k_conditional_await_message, span);
  }
  auto &setup = in_task ? *hoist_ : local_setup;
  const auto symbol = mint();
  names_.insert_or_assign(symbol, "<on task>");
  const auto task_ref = [&]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, task_type, symbol, "<on task>");
  };
  setup.push_back(hir::make<hir_let>(span, symbol, "<on task>", std::move(made)));
  auto spawn_args = ptr_vec<hir_expr>{};
  spawn_args.push_back(task_ref());
  spawn_args.push_back(pool_literal(
      checked_.types, span, static_cast<runtime::scheduler::pool>(pool)));
  setup.push_back(hir::make<hir_expr_stmt>(
      span, runtime_call(span, "rt_task_spawn",
                         interning_types().builtin("unit"),
                         std::move(spawn_args))));
  if (in_task) {
    auto held = ptr_vec<hir_expr>{};
    held.push_back(task_ref());
    if (auto waited = task_wait_into(setup, task_ref(), wait_key, span,
                                     /*all=*/false, std::move(held));
        !waited.has_value()) {
      return std::unexpected(waited.error());
    }
  }
  auto drops = ptr_vec<hir_expr>{};
  drops.push_back(task_ref());
  auto result = read_then_drop_tasks(
      setup,
      hir::make<hir_generator_next>(span, type, task_ref(),
                                    /*await_task=*/true),
      std::move(drops), type, span);
  if (in_task) {
    return result;
  }
  local_setup.push_back(hir::make<hir_expr_stmt>(span, std::move(result)));
  return ptr<hir_expr>(
      hir::make<hir_block>(span, type, std::move(local_setup)));
}

auto core_lowerer::await_group(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto is_par = source.target.name == "await_par";
  const auto span = source.span;
  const auto type = hir_type(source.type);
  if (source.args.empty()) {
    return refuse("par or race with no task", span);
  }
  const auto in_task = in_task_body_;
  if (in_task && cannot_hoist()) {
    return refuse_feature("await where it cannot go ahead of its statement", k_conditional_await_message, span);
  }
  auto local_setup = ptr_vec<hir_node>{};
  auto &setup = in_task ? *hoist_ : local_setup;
  auto &types = interning_types();
  const auto unit_type = types.builtin("unit");
  const auto bool_type = checked_.types.bool_type();

  // Each branch's task in a local, then every one spawned, so they overlap.
  auto symbols = std::vector<symbol_id>{};
  auto task_types = std::vector<type_id>{};
  auto spans = std::vector<source_span>{};
  for (const auto &branch : source.args) {
    auto task = expr(*branch);
    if (!task.has_value()) {
      return task;
    }
    const auto symbol = mint();
    names_.insert_or_assign(symbol, "<group task>");
    symbols.push_back(symbol);
    task_types.push_back(hir_type(branch->type));
    spans.push_back(branch->span);
    setup.push_back(
        hir::make<hir_let>(branch->span, symbol, "<group task>", std::move(*task)));
  }
  const auto count = symbols.size();
  for (std::size_t i = 0; i < count; ++i) {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(hir::make<hir_local_ref>(spans[i], task_types[i], symbols[i],
                                            "<group task>"));
    args.push_back(
        pool_literal(checked_.types, span, runtime::scheduler::pool::io));
    setup.push_back(hir::make<hir_expr_stmt>(
        span, runtime_call(span, "rt_task_spawn", unit_type, std::move(args))));
  }

  // The tasks as one tuple, the block of slots the runtime's waits read.
  auto members = ptr_vec<hir_expr>{};
  for (std::size_t i = 0; i < count; ++i) {
    members.push_back(hir::make<hir_local_ref>(spans[i], task_types[i],
                                               symbols[i], "<group task>"));
  }
  const auto group_type = types.tuple_of(task_types);
  const auto group_name =
      std::string(is_par ? "<par tasks>" : "<race tasks>");
  const auto group = mint();
  names_.insert_or_assign(group, group_name);
  setup.push_back(hir::make<hir_let>(
      span, group, group_name,
      hir::make<hir_tuple>(span, group_type, std::move(members))));
  const auto member = [&](std::size_t i) -> ptr<hir_expr> {
    return hir::make<hir_tuple_index>(
        span, task_types[i],
        hir::make<hir_local_ref>(span, group_type, group, group_name), i);
  };
  if (in_task) {
    auto held = ptr_vec<hir_expr>{};
    for (std::size_t i = 0; i < count; ++i) {
      held.push_back(member(i));
    }
    if (auto waited = task_wait_into(
            setup, hir::make<hir_local_ref>(span, group_type, group, group_name),
            source.origin, span, /*all=*/is_par, std::move(held));
        !waited.has_value()) {
      return std::unexpected(waited.error());
    }
  } else {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(hir::make<hir_local_ref>(span, group_type, group, group_name));
    args.push_back(hir::make<hir_literal>(span, checked_.types.usize_type(),
                                          token_kind::int_lit,
                                          std::to_string(count)));
    setup.push_back(hir::make<hir_expr_stmt>(
        span, runtime_call(span,
                           is_par ? "rt_task_block_on_all"
                                  : "rt_task_block_on_any",
                           unit_type, std::move(args))));
  }

  auto result = ptr<hir_expr>{};
  if (is_par) {
    const auto &result_entry = checked_.types.entry(type);
    if (result_entry.kind != type_kind::tuple_kind ||
        result_entry.args.size() != count) {
      return refuse("par whose type is not the tuple of its results", span);
    }
    auto elements = ptr_vec<hir_expr>{};
    auto drops = ptr_vec<hir_expr>{};
    for (std::size_t i = 0; i < count; ++i) {
      elements.push_back(hir::make<hir_generator_next>(
          span, result_entry.args[i], member(i), /*await_task=*/true));
      drops.push_back(member(i));
    }
    result = read_then_drop_tasks(
        setup, hir::make<hir_tuple>(span, type, std::move(elements)),
        std::move(drops), type, span);
  } else {
    // The first finished branch in source order wins; every task is then
    // cancelled (a finished one ignores it) and dropped.
    const auto read_branch = [&](std::size_t i) -> ptr<hir_block> {
      auto stmts = ptr_vec<hir_node>{};
      stmts.push_back(hir::make<hir_expr_stmt>(
          span, hir::make<hir_generator_next>(span, type, member(i),
                                              /*await_task=*/true)));
      return hir::make<hir_block>(span, type, std::move(stmts));
    };
    auto picks = std::vector<hir_if_branch>{};
    for (std::size_t i = 0; i + 1 < count; ++i) {
      auto args = ptr_vec<hir_expr>{};
      args.push_back(member(i));
      picks.push_back(hir_if_branch{
          .condition = runtime_call(span, "rt_task_is_finished", bool_type,
                                    std::move(args)),
          .body = read_branch(i)});
    }
    auto last = read_branch(count - 1);
    auto pick = ptr<hir_expr>{};
    if (picks.empty()) {
      pick = std::move(last);
    } else {
      pick = hir::make<hir_if>(span, type, std::move(picks), std::move(last));
    }
    const auto winner = mint();
    names_.insert_or_assign(winner, "<race winner>");
    setup.push_back(
        hir::make<hir_let>(span, winner, "<race winner>", std::move(pick)));
    for (std::size_t i = 0; i < count; ++i) {
      auto args = ptr_vec<hir_expr>{};
      args.push_back(member(i));
      setup.push_back(hir::make<hir_expr_stmt>(
          span,
          runtime_call(span, "rt_task_cancel", bool_type, std::move(args))));
    }
    for (std::size_t i = 0; i < count; ++i) {
      setup.push_back(hir::make<hir_expr_stmt>(
          span,
          hir::make<hir_coroutine_cancel>(span, k_unknown_type, member(i))));
    }
    result = hir::make<hir_local_ref>(span, type, winner, "<race winner>");
  }
  if (in_task) {
    return result;
  }
  local_setup.push_back(hir::make<hir_expr_stmt>(span, std::move(result)));
  return ptr<hir_expr>(
      hir::make<hir_block>(span, type, std::move(local_setup)));
}

auto core_lowerer::explicit_references(hir_function &made)
    -> core_result<void> {
  auto temporary_drop = std::optional<unsupported>{};
  make_references_explicit(
      made, checked_, [this]() -> symbol_id { return mint(); },
      [&](const temporary &temp, ptr_vec<hir_node> &out) -> void {
        if (temporary_drop.has_value()) {
          return;
        }
        if (auto dropped = drops_.drop_temporary(temp, out); !dropped) {
          temporary_drop = refuse_drop(dropped.error()).error();
        }
      },
      [this](const void *origin) -> std::optional<const void *> {
        return drops_.temporary_end(origin);
      });
  if (temporary_drop.has_value()) {
    return std::unexpected(*temporary_drop);
  }
  return {};
}

auto core_lowerer::task_function(
    const void *key, const void *start_key, std::string name,
    const std::function<core_result<ptr<hir_block>>()> &lower_body,
    type_id declared, type_id task_type, source_span span,
    const std::vector<hir_param> &outer, ptr_vec<hir_expr> carried)
    -> core_result<ptr<hir_expr>> {
  static const auto no_captures =
      std::vector<semantic::ownership::task_capture>{};
  const auto found = drops_.schedule().task_captures.find(key);
  const auto &captures = found != drops_.schedule().task_captures.end()
                             ? found->second
                             : no_captures;

  // The body is a function of its own: nothing it does leaves through the
  // enclosing function's exits or suspension points.
  const auto enclosing_task = std::exchange(in_task_body_, true);
  auto *const enclosing_hoist = std::exchange(hoist_, nullptr);
  const auto enclosing_conditional = std::exchange(conditional_depth_, 0);
  const auto enclosing_value_branch = std::exchange(value_branch_depth_, 0);
  const auto *const enclosing_body = std::exchange(function_body_, nullptr);
  auto enclosing_crews = std::exchange(open_crews_, {});
  const auto restore = [&] -> void {
    open_crews_ = std::move(enclosing_crews);
    in_task_body_ = enclosing_task;
    hoist_ = enclosing_hoist;
    conditional_depth_ = enclosing_conditional;
    value_branch_depth_ = enclosing_value_branch;
    function_body_ = enclosing_body;
  };

  // The parameters' scope. A capture the task owns is the enclosing local,
  // declared again here, which is how its drops name it.
  push_scope();
  auto prelude = ptr_vec<hir_node>{};
  auto owned = std::unordered_set<symbol_id>{};
  for (const auto &capture : captures) {
    if (!capture.moved) {
      continue;
    }
    // The capture's first reference in the body declares it in the task
    // (`task_capture::node`); its symbol is the parameter it arrives in.
    const auto param = std::ranges::find_if(
        outer, [&](const hir_param &p) -> bool { return p.name == capture.name; });
    const auto symbol = param != outer.end()
                            ? std::optional<symbol_id>(param->symbol)
                            : whole_local(capture.node);
    if (!symbol.has_value()) {
      pop_scope();
      restore();
      return refuse(std::format("task block capture `{}` that is not a local",
                                capture.name),
                    span);
    }
    owned.insert(*symbol);
    own(capture.node, *symbol);
    drops_.declare_flags(capture.node, span, prelude);
  }
  // A task dropped before it first runs returns at once.
  auto cancel = cancel_check(start_key, span, std::move(carried));
  if (!cancel.has_value()) {
    pop_scope();
    restore();
    return std::unexpected(cancel.error());
  }
  prelude.push_back(std::move(*cancel));

  auto block = lower_body();
  if (block.has_value() &&
      ((*block)->stmts.empty() || !always_exits(*(*block)->stmts.back()))) {
    const auto value_typed =
        declared != k_unknown_type && !checked_.types.is_unit(declared);
    auto dropped =
        value_typed ? drops_.emit_tail_scope_exit_drops(key, (*block)->stmts)
                    : drops_.emit_scope_exit_drops(key, (*block)->stmts);
    if (!dropped.has_value()) {
      pop_scope();
      restore();
      return refuse_drop(dropped.error());
    }
  }
  pop_scope();
  if (!block.has_value()) {
    restore();
    return std::unexpected(block.error());
  }
  // The body ends by handing over its result: a trailing value, or `unit`.
  auto &stmts = (*block)->stmts;
  if (stmts.empty() || !always_returns(*stmts.back())) {
    auto value = ptr<hir_expr>{};
    if (!stmts.empty() && stmts.back()->kind == hir_node_kind::hir_expr_stmt &&
        !checked_.types.is_unit(declared)) {
      value = std::move(dynamic_cast<hir_expr_stmt &>(*stmts.back()).expr);
      stmts.pop_back();
    } else {
      value = hir::make<hir_literal>(span, interning_types().builtin("unit"),
                                     token_kind::kw_unit, "unit");
    }
    stmts.push_back(hir::make<hir_yield>(span, std::move(value)));
    stmts.push_back(hir::make<hir_return>(span, nullptr));
  }
  restore();
  for (auto &stmt : stmts) {
    prelude.push_back(std::move(stmt));
  }
  stmts = std::move(prelude);

  // Every local of the enclosing function the body reads is a parameter:
  // lent, unless the task owns it.
  auto params = std::vector<hir_param>{};
  auto args = ptr_vec<hir_expr>{};
  auto lent_args = std::vector<std::size_t>{};
  for (const auto symbol : free_variables(**block)) {
    const auto *local = [&]() -> const hir_param * {
      for (const auto &candidate : outer) {
        if (candidate.symbol == symbol) {
          return &candidate;
        }
      }
      return nullptr;
    }();
    if (local == nullptr) {
      // A function or a global, named directly.
      continue;
    }
    const auto lent = !owned.contains(symbol);
    if (lent) {
      lent_args.push_back(args.size());
    }
    params.push_back(hir_param{.symbol = symbol,
                               .name = local->name,
                               .type = local->type,
                               .lent = lent});
    args.push_back(
        hir::make<hir_local_ref>(span, local->type, symbol, local->name));
  }
  auto function =
      hir::make<hir_function>(span, name, std::move(params), task_type,
                              std::move(*block), coroutine_kind::task, declared);
  if (auto finished = explicit_references(*function); !finished) {
    return std::unexpected(finished.error());
  }
  nested_.push_back(std::move(function));
  auto call = hir::make<hir_call>(
      span, task_type,
      hir::make<hir_local_ref>(span, k_unknown_type, callee_symbol(name), name),
      std::move(args));
  call->lent_args = std::move(lent_args);
  return ptr<hir_expr>(std::move(call));
}

auto core_lowerer::task_block(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  if (source.args.size() != 1 ||
      source.args.front()->kind != core::node_kind::lambda) {
    return refuse("task_block without its body", source.span);
  }
  const auto &body = static_cast<const core::lambda &>(*source.args.front());
  const auto *key = body.origin;
  if (key == nullptr || body.body == nullptr) {
    return refuse("task_block without its block", source.span);
  }
  // Inside the body a capture is the outer local itself.
  auto outer = std::vector<hir_param>{};
  for (const auto &capture : body.captures) {
    const auto &root = capture.from.root;
    if (root.kind != core::place_root::origin::local ||
        !capture.from.path.empty() || aliases_.contains(root.symbol)) {
      return refuse("task block capture of a place that is not a local",
                    source.span);
    }
    const auto found = symbols_.find(root.symbol);
    if (found == symbols_.end()) {
      return refuse("task block capture of a local declared outside the "
                    "supported forms",
                    source.span);
    }
    symbols_.insert_or_assign(capture.symbol, found->second);
    outer.push_back(hir_param{.symbol = found->second,
                              .name = root.name,
                              .type = hir_type(root.type)});
  }
  const auto declared = hir_type(body.return_type);
  if (body.task_start == nullptr) {
    return refuse("task_block without its start key", source.span);
  }
  return task_function(
      key, body.task_start,
      std::format("<task block {}@{}>", function_name_, body.span.start),
      [&] { return block(*body.body, declared); }, declared,
      hir_type(source.type), source.span, outer);
}

auto core_lowerer::on_task(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto span = source.span;
  if (source.args.size() != 1 || source.origin == nullptr) {
    return refuse("on with no task", span);
  }
  const auto where = source.target.name == "on_io"
                         ? runtime::scheduler::pool::io
                         : runtime::scheduler::pool::cpu;
  const auto task_type = hir_type(source.type);
  const auto declared = task_result_of(interning_types(), task_type);
  if (!declared.has_value()) {
    return refuse("on whose type is not a task", span);
  }
  // The task that runs on the pool is made here; the wrapper only takes it
  // along.
  const auto &sender = *source.args.front();
  auto made = intrinsic_named(sender, "task_block")
                  ? task_block(static_cast<const core::call &>(sender))
                  : expr(sender);
  if (!made.has_value()) {
    return made;
  }
  const auto held = mint();
  names_.insert_or_assign(held, "<on task>");
  // The drop schedule names the wrapper by the `on(...)`'s context.
  const auto *key = source.context_origin;
  if (key == nullptr) {
    return refuse("on with no context key", span);
  }
  auto carried = ptr_vec<hir_expr>{};
  carried.push_back(hir::make<hir_local_ref>(span, task_type, held, "<on task>"));
  auto outer = std::vector<hir_param>{
      hir_param{.symbol = held, .name = "<capture>", .type = task_type}};
  auto wrapper = task_function(
      key, key, std::format("<on {}@{}>", function_name_, span.start),
      [&]() -> core_result<ptr<hir_block>> {
        auto stmts = ptr_vec<hir_node>{};
        auto *const enclosing = std::exchange(hoist_, &stmts);
        auto value = spawn_and_wait(
            hir::make<hir_local_ref>(span, task_type, held, "<on task>"),
            task_type, static_cast<std::size_t>(where), *declared, key, span);
        hoist_ = enclosing;
        if (!value.has_value()) {
          return std::unexpected(value.error());
        }
        stmts.push_back(hir::make<hir_expr_stmt>(span, std::move(*value)));
        return hir::make<hir_block>(span, *declared, std::move(stmts));
      },
      *declared, task_type, span, outer, std::move(carried));
  if (!wrapper.has_value()) {
    return wrapper;
  }
  auto stmts = ptr_vec<hir_node>{};
  stmts.push_back(hir::make<hir_let>(span, held, "<on task>", std::move(*made)));
  stmts.push_back(hir::make<hir_expr_stmt>(span, std::move(*wrapper)));
  return ptr<hir_expr>(hir::make<hir_block>(span, task_type, std::move(stmts)));
}

auto core_lowerer::crew_block(const core::expr &source) -> const core::block * {
  if (source.kind != core::node_kind::block) {
    return nullptr;
  }
  const auto &made = static_cast<const core::block &>(source);
  if (made.statements.size() != 3 ||
      made.statements.front()->kind != core::node_kind::let) {
    return nullptr;
  }
  const auto &bound = static_cast<const core::let &>(*made.statements.front());
  return bound.value != nullptr && intrinsic_named(*bound.value, "crew_new")
             ? &made
             : nullptr;
}

auto core_lowerer::crew_into(const core::block &source, type_id type,
                             ptr_vec<hir_node> &out)
    -> core_result<ptr<hir_expr>> {
  auto &types = interning_types();
  const auto unit_type = types.builtin("unit");
  const auto crew_type = types.builtin_generic("crew", {});
  const auto has_value = type != k_unknown_type && type != unit_type;
  const auto span = source.span;
  const auto &bound = static_cast<const core::let &>(*source.statements[0]);
  // The body: the value of `let <crew result> = body`, or `body` alone.
  const auto *inner = [&]() -> const core::expr * {
    const auto &held = *source.statements[1];
    if (held.kind == core::node_kind::let) {
      return static_cast<const core::let &>(held).value.get();
    }
    if (held.kind == core::node_kind::expr_stmt) {
      return static_cast<const core::expr_stmt &>(held).value.get();
    }
    return nullptr;
  }();
  if (inner == nullptr || inner->kind != core::node_kind::block) {
    return refuse("crew without its body", span);
  }

  // let c = <new crew>;  let r = { body };  <wait for c's tasks>;  <free c>
  push_scope();
  const auto crew = declare(bound.symbol, bound.name);
  out.push_back(hir::make<hir_let>(
      span, crew, bound.name,
      runtime_call(span, "rt_crew_new", crew_type, ptr_vec<hir_expr>{})));
  open_crews_.push_back(crew);
  auto body = block(static_cast<const core::block &>(*inner),
                    has_value ? type : unit_type);
  open_crews_.pop_back();
  pop_scope();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  auto result = ptr<hir_expr>{};
  const auto keep = [&](ptr<hir_expr> value) {
    const auto symbol = mint();
    names_.insert_or_assign(symbol, "<crew result>");
    out.push_back(
        hir::make<hir_let>(span, symbol, "<crew result>", std::move(value)));
    result = hir::make<hir_local_ref>(span, type, symbol, "<crew result>");
  };
  if (in_task_body_) {
    // The body can wait, and a wait is only a suspension point among the
    // statements of the task body, so its statements join this list.
    auto &stmts = (*body)->stmts;
    auto tail = ptr<hir_expr>{};
    if (has_value && !stmts.empty() &&
        stmts.back()->kind == hir_node_kind::hir_expr_stmt) {
      tail = std::move(static_cast<hir_expr_stmt &>(*stmts.back()).expr);
      stmts.pop_back();
    }
    for (auto &stmt : stmts) {
      out.push_back(std::move(stmt));
    }
    if (has_value) {
      if (tail == nullptr) {
        return refuse("crew whose value is not its last expression", span);
      }
      keep(std::move(tail));
    }
  } else if (has_value) {
    keep(std::move(*body));
  } else {
    out.push_back(hir::make<hir_expr_stmt>(span, std::move(*body)));
  }

  auto end = crew_end_into(crew, span, out);
  if (!end.has_value()) {
    return std::unexpected(end.error());
  }
  if (in_task_body_) {
    // The value is not bound to a name yet, so a cancel here releases the
    // tasks it holds and drops the rest of it.
    auto held = ptr_vec<hir_expr>{};
    auto drops = ptr_vec<hir_node>{};
    if (result != nullptr) {
      const auto symbol = static_cast<const hir_local_ref &>(*result).symbol;
      const auto result_ref = [&, symbol]() -> ptr<hir_expr> {
        return hir::make<hir_local_ref>(span, type, symbol, "<crew result>");
      };
      const auto drop_part = [&](const place_fn &part,
                                 type_id part_type) -> core_result<void> {
        if (!checked_.drop.drop_plans.contains(part_type)) {
          return {};
        }
        if (auto dropped = drops_.build_drop_calls(part, part_type, span, drops);
            !dropped.has_value()) {
          return refuse_drop(dropped.error());
        }
        return {};
      };
      const auto &entry = checked_.types.entry(type);
      if (is_task_type(checked_.types, type)) {
        held.push_back(result_ref());
      } else if (entry.kind == type_kind::tuple_kind) {
        for (std::size_t i = 0; i < entry.args.size(); ++i) {
          const auto part_type = entry.args[i];
          const auto part = [&, part_type, i]() -> ptr<hir_expr> {
            return hir::make<hir_tuple_index>(span, part_type, result_ref(), i);
          };
          if (is_task_type(checked_.types, part_type)) {
            held.push_back(part());
          } else if (auto ok = drop_part(part, part_type); !ok) {
            return std::unexpected(ok.error());
          }
        }
      } else if (auto ok = drop_part(result_ref, type); !ok) {
        return std::unexpected(ok.error());
      }
    }
    auto cancel =
        cancel_check(source.origin, span, std::move(held), std::move(drops));
    if (!cancel.has_value()) {
      return std::unexpected(cancel.error());
    }
    out.push_back(std::move(*cancel));
  }
  return result;
}

auto core_lowerer::crew_end_into(symbol_id crew, source_span span,
                                 ptr_vec<hir_node> &out) -> core_result<void> {
  auto &types = interning_types();
  const auto crew_type = types.builtin_generic("crew", {});
  const auto unit_type = types.builtin("unit");
  const auto crew_ref = [&]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, crew_type, crew, "<crew>");
  };
  // A task waits for the crew's tasks at a suspension point; anything else
  // blocks the thread.
  if (in_task_body_) {
    out.push_back(hir::make<hir_yield>(span, crew_ref(), /*awaiting_task=*/true,
                                       /*awaiting_all=*/false));
  } else {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(crew_ref());
    out.push_back(hir::make<hir_expr_stmt>(
        span, runtime_call(span, "rt_crew_join", unit_type, std::move(args))));
  }
  auto args = ptr_vec<hir_expr>{};
  args.push_back(crew_ref());
  out.push_back(hir::make<hir_expr_stmt>(
      span, runtime_call(span, "rt_crew_free", unit_type, std::move(args))));
  return {};
}

auto core_lowerer::crew_end_call_into(const core::call &source,
                                      ptr_vec<hir_node> &out)
    -> core_result<void> {
  if (source.args.size() != 1 ||
      source.args.front()->kind != core::node_kind::use) {
    return refuse("crew_end of no crew", source.span);
  }
  const auto &root = static_cast<const core::use &>(*source.args.front()).from.root;
  const auto found = symbols_.find(root.symbol);
  if (root.kind != core::place_root::origin::local || found == symbols_.end()) {
    return refuse("crew_end of no crew", source.span);
  }
  if (in_task_body_ && (value_branch_depth_ > 0 || conditional_depth_ > 0)) {
    return refuse_feature("crew exit where it cannot be a statement", k_crew_exit_message, source.span);
  }
  return crew_end_into(found->second, source.span, out);
}

auto core_lowerer::crew_errors(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto span = source.span;
  if (source.args.size() != 3 || source.target.generic_args.size() != 1 ||
      source.args[1]->kind != core::node_kind::function_ref ||
      source.args[2]->kind != core::node_kind::function_ref) {
    return refuse("crew_errors without its list functions", span);
  }
  const auto type = hir_type(source.type);
  const auto task_type = source.target.generic_args.front();
  const auto &task_entry = checked_.types.entry(task_type);
  if (task_entry.args.size() < 2) {
    return refuse("crew_errors of no task type", span);
  }
  auto &types = interning_types();
  const auto error_type = task_entry.args[1];
  const auto result_type =
      types.builtin_generic("result", {task_entry.args[0], error_type});
  const auto unit_type = types.builtin("unit");
  const auto usize_type = checked_.types.usize_type();
  const auto bool_type = checked_.types.bool_type();
  const auto in_task = in_task_body_;
  if (in_task && cannot_hoist()) {
    return refuse_feature("await where it cannot go ahead of its statement", k_conditional_await_message, span);
  }
  auto crew_value = receiver_operand(*source.args.front());
  if (!crew_value.has_value()) {
    return crew_value;
  }
  const auto crew_type = (*crew_value)->type;
  // The crew's own name is used where it stands: a second name would be a
  // second owner of the handle, which the crew's join still uses.
  const auto *named = (*crew_value)->kind == hir_node_kind::hir_local_ref
                          ? &static_cast<const hir_local_ref &>(**crew_value)
                          : nullptr;
  const auto crew_symbol = named != nullptr ? named->symbol : mint();
  const auto crew_name =
      named != nullptr ? named->name : std::string("<errors crew>");
  const auto crew_ref = [&]() -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, crew_type, crew_symbol, crew_name);
  };
  auto stmts = ptr_vec<hir_node>{};
  auto &prologue = in_task ? *hoist_ : stmts;
  if (named == nullptr) {
    names_.insert_or_assign(crew_symbol, crew_name);
    prologue.push_back(
        hir::make<hir_let>(span, crew_symbol, crew_name, std::move(*crew_value)));
  }
  if (in_task) {
    if (auto waited = task_wait_into(prologue, crew_ref(), source.origin, span,
                                     /*all=*/false, {});
        !waited.has_value()) {
      return std::unexpected(waited.error());
    }
  } else {
    auto args = ptr_vec<hir_expr>{};
    args.push_back(crew_ref());
    prologue.push_back(hir::make<hir_expr_stmt>(
        span, runtime_call(span, "rt_crew_join", unit_type, std::move(args))));
  }
  const auto call_of = [&](const core::expr &named_fn,
                           ptr_vec<hir_expr> args) -> ptr<hir_expr> {
    const auto &target = static_cast<const core::function_ref &>(named_fn).target;
    const auto result = checked_.types.entry(target.signature).result;
    auto made = hir::make<hir_call>(span, result,
                                    function_callee(target, span),
                                    std::move(args));
    made->target = target.decl;
    return made;
  };
  const auto local = [&](symbol_id symbol, type_id local_type,
                         const char *name) -> ptr<hir_expr> {
    return hir::make<hir_local_ref>(span, local_type, symbol, name);
  };
  const auto usize_literal = [&](const char *text) -> ptr<hir_expr> {
    return hir::make<hir_literal>(span, usize_type, token_kind::int_lit, text);
  };
  const auto acc = mint();
  const auto count = mint();
  const auto index = mint();
  const auto task = mint();
  const auto outcome = mint();
  stmts.push_back(hir::make<hir_let>(span, acc, "<errors result>",
                                     call_of(*source.args[1], {}),
                                     /*mut=*/true));
  auto len_args = ptr_vec<hir_expr>{};
  len_args.push_back(crew_ref());
  stmts.push_back(hir::make<hir_let>(
      span, count, "<errors count>",
      runtime_call(span, "rt_crew_len", usize_type, std::move(len_args)),
      /*mut=*/false));
  stmts.push_back(hir::make<hir_let>(span, index, "<errors index>",
                                     usize_literal("0"), /*mut=*/true));
  auto condition = hir::make<hir_binary>(
      span, bool_type, ast::binary_op::lt,
      local(index, usize_type, "<errors index>"),
      local(count, usize_type, "<errors count>"));
  auto body_stmts = ptr_vec<hir_node>{};
  auto task_args = ptr_vec<hir_expr>{};
  task_args.push_back(crew_ref());
  task_args.push_back(local(index, usize_type, "<errors index>"));
  body_stmts.push_back(hir::make<hir_let>(
      span, task, "<errors task>",
      runtime_call(span, "rt_crew_task", task_type, std::move(task_args))));
  body_stmts.push_back(hir::make<hir_let>(
      span, outcome, "<errors outcome>",
      hir::make<hir_generator_next>(span, result_type,
                                    local(task, task_type, "<errors task>"),
                                    /*await_task=*/true)));
  auto push_args = ptr_vec<hir_expr>{};
  push_args.push_back(local(acc, type, "<errors result>"));
  push_args.push_back(hir::make<hir_variant_payload>(
      span, error_type, local(outcome, result_type, "<errors outcome>"), "err",
      std::size_t{0}));
  auto err_stmts = ptr_vec<hir_node>{};
  err_stmts.push_back(hir::make<hir_expr_stmt>(
      span, call_of(*source.args[2], std::move(push_args))));
  auto err_args = ptr_vec<hir_pattern>{};
  err_args.push_back(hir::make<hir_wildcard_pattern>(span));
  auto arms = std::vector<hir_match_arm>{};
  arms.push_back(hir_match_arm{
      .pattern = hir::make<hir_constructor_pattern>(span, "err",
                                                    std::move(err_args)),
      .guard = nullptr,
      .body = hir::make<hir_block>(span, k_unknown_type, std::move(err_stmts))});
  arms.push_back(hir_match_arm{
      .pattern = hir::make<hir_wildcard_pattern>(span),
      .guard = nullptr,
      .body = hir::make<hir_block>(span, k_unknown_type, ptr_vec<hir_node>{})});
  body_stmts.push_back(hir::make<hir_expr_stmt>(
      span, hir::make<hir_match>(span, k_unknown_type,
                                 local(outcome, result_type, "<errors outcome>"),
                                 mint(), std::move(arms))));
  auto step = ptr_vec<hir_node>{};
  step.push_back(hir::make<hir_assign>(span, ast::assign_op::add_assign,
                                       local(index, usize_type, "<errors index>"),
                                       usize_literal("1")));
  stmts.push_back(hir::make<hir_while>(
      span, std::move(condition),
      hir::make<hir_block>(span, k_unknown_type, std::move(body_stmts)),
      hir::make<hir_block>(span, k_unknown_type, std::move(step))));
  stmts.push_back(
      hir::make<hir_expr_stmt>(span, local(acc, type, "<errors result>")));
  return ptr<hir_expr>(hir::make<hir_block>(span, type, std::move(stmts)));
}

auto core_lowerer::task_yield(const core::call &source)
    -> core_result<ptr<hir_expr>> {
  const auto span = source.span;
  auto unit = ptr<hir_expr>(
      hir::make<hir_block>(span, hir_type(source.type), ptr_vec<hir_node>{}));
  if (!source.args.empty()) {
    return refuse("task_yield with an operand", span);
  }
  // The entry `async def main` is no task: nothing queues it, and nothing
  // can cancel it, so there is nothing to give up or check.
  if (!in_task_body_) {
    return unit;
  }
  if (cannot_hoist()) {
    return refuse_feature("await where it cannot go ahead of its statement",
                          k_conditional_await_message, span);
  }
  if (auto waited = task_wait_into(*hoist_, nullptr, source.origin, span,
                                   /*all=*/false, {});
      !waited.has_value()) {
    return std::unexpected(waited.error());
  }
  return unit;
}

auto core_lowerer::task_intrinsic(const core::call &source)
    -> std::optional<core_result<ptr<hir_expr>>> {
  const auto &name = source.target.name;
  const auto type = hir_type(source.type);
  if (name == "task_block") {
    return task_block(source);
  }
  if (name == "task_yield") {
    return task_yield(source);
  }
  if (name == "on_cpu" || name == "on_io") {
    return on_task(source);
  }
  if (name == "await_par" || name == "await_race") {
    return await_group(source);
  }
  if (name == "crew_errors") {
    return crew_errors(source);
  }
  if (name == "task_spawn" || name == "crew_spawn" || name == "task_get") {
    if (source.args.empty()) {
      return refuse(std::format("{} with no operand", name), source.span);
    }
    auto object = receiver_operand(*source.args.front());
    if (!object.has_value()) {
      return object;
    }
    if (name == "task_get") {
      auto task_type = (*object)->type;
      while (checked_.types.entry(task_type).kind == type_kind::ref_kind) {
        task_type = checked_.types.entry(task_type).result;
      }
      return task_read(source.origin, source.span, source.args.front()->span,
                       type, task_type, std::move(*object),
                       /*consumes=*/false);
    }
    auto args = ptr_vec<hir_expr>{};
    args.push_back(std::move(*object));
    if (name == "task_spawn") {
      args.push_back(
          pool_literal(checked_.types, source.span, runtime::scheduler::pool::io));
      return runtime_call(source.span, "rt_task_spawn", type, std::move(args));
    }
    if (source.args.size() != 2) {
      return refuse("crew_spawn with no task", source.span);
    }
    auto task = expr(*source.args[1]);
    if (!task.has_value()) {
      return task;
    }
    args.push_back(std::move(*task));
    return runtime_call(source.span, "rt_crew_spawn", type, std::move(args));
  }
  return std::nullopt;
}

} // namespace cinder::hir
