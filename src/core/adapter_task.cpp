// Concurrency forms (spec/unified-checker-plan.md, phase 2).
//
// The core has no node for a task or a crew. Each form is a `call` with an
// `intrinsic` callee named after the operation, taking a lambda (for a task
// body, with its captures explicit) or the values it works on:
//
//   async: body             task_block(fn() -> T: body)       -> task[...]
//   on(cpu|pool, s)         on_cpu(s)                         -> task[...]
//   on(io): body            on_io(task_block(fn() -> T: body))
//   await on(...)           await on_X(...)
//   await par: a, b         await_par(a, b)                   -> (A, B)
//   await race: a, b        await_race(a, b)                  -> R
//   crew k: body            { let k = crew_new(); body; crew_end(k) }
//   t.spawn()               task_spawn(&t)
//   k.spawn(t)              crew_spawn(&k, t)
//   t.get()                 task_get(&t)
//   k.errors()              crew_errors(&k)                   -> list[E]
//
// A task body's captures come from the ownership checker's `task_capture`
// records: one it consumes is moved in, any other is lent (a copy for a copy
// type). A `return`, `break`, `continue` or `?` that leaves a crew ends it
// first, as lowering's `emit_crew_exits` does.

#include <format>
#include <string>
#include <utility>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

namespace cinder::core {

using semantic::type_kind;

auto body_builder::task_block_of(const void *key,
                                 const std::vector<ast::ptr<ast::node>> &body,
                                 type_id declared, source_span span)
    -> result<ptr<expr>> {
  auto &types = checked_.types;
  const auto fn_type = types.fn_of({}, declared);
  auto made = make_node<core::lambda>(span, fn_type);
  made->return_type = declared;

  push_scope();
  lambdas_.push_back(lambda_frame{.source = nullptr,
                                  .task_key = key,
                                  .scope_base = scopes_.size() - 1,
                                  .captures = {},
                                  .unlisted = std::nullopt});
  const auto saved_body_type = std::exchange(body_type_, declared);
  auto saved_posts = std::exchange(post_contracts_, {});
  auto saved_crews = std::exchange(open_crews_, {});
  const auto saved_outside = std::exchange(crews_outside_loop_, 0);
  push_scope();
  auto built = block_of(body, span, declared);
  body_type_ = saved_body_type;
  post_contracts_ = std::move(saved_posts);
  open_crews_ = std::move(saved_crews);
  crews_outside_loop_ = saved_outside;
  auto frame = std::move(lambdas_.back());
  lambdas_.pop_back();
  while (scopes_.size() > frame.scope_base) {
    pop_scope();
  }
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  made->captures = std::move(frame.captures);
  made->body = std::move(*built);
  auto args = ptr_vec<expr>{};
  args.push_back(ptr<expr>(std::move(made)));
  return intrinsic_call("task_block", task_of(declared), std::move(args), {},
                        span);
}

auto body_builder::task_result_type(type_id task_type, source_span span)
    -> result<type_id> {
  const auto &task = checked_.types.entry(task_type);
  if (task.kind != type_kind::builtin_generic_kind || task.name != "task" ||
      task.args.size() < 2) {
    return fail("missing record: types",
                std::format("a task form typed `{}`, which is not a task",
                            checked_.types.display(task_type)),
                span);
  }
  const auto fallible = checked_.types.entry(task.args[1]).name != "never";
  return fallible ? checked_.types.builtin_generic(
                        "result", {task.args[0], task.args[1]})
                  : task.args[0];
}

auto body_builder::on_task_of(const ast::on_expr &on, type_id declared,
                              type_id task_type) -> result<ptr<expr>> {
  const auto *named = ast::as<const ast::named_type>(on.context_type.get());
  const auto target =
      named != nullptr && named->path.size() == 1 ? named->path.front() : "";
  if (target != "io" && target != "cpu" && target != "pool") {
    return fail("on_expr", "an `on(...)` context other than cpu, pool or io",
                on.span);
  }
  auto made = ptr<expr>{};
  if (on.sender != nullptr) {
    CORE_TRY(sender, value_of(*on.sender));
    made = std::move(sender);
  } else {
    CORE_TRY(block, task_block_of(&on, on.body, declared, on.span));
    made = std::move(block);
  }
  auto args = ptr_vec<expr>{};
  args.push_back(std::move(made));
  return intrinsic_call(target == "io" ? "on_io" : "on_cpu", task_type,
                        std::move(args), {}, on.span);
}

auto body_builder::await_task_form(const ast::await_expr &awaited)
    -> std::optional<result<ptr<expr>>> {
  const auto &operand = *awaited.operand;
  const auto kind = operand.kind;
  if (kind != ast::node_kind::par_expr && kind != ast::node_kind::race_expr &&
      kind != ast::node_kind::on_expr) {
    return std::nullopt;
  }
  return [&]() -> result<ptr<expr>> {
    CORE_TRY(type, type_of(awaited));
    if (kind == ast::node_kind::on_expr) {
      const auto &on = ast::cast<const ast::on_expr>(operand);
      auto task_type = task_of(type);
      if (on.sender != nullptr) {
        CORE_TRY(sender_type, type_of(*on.sender));
        task_type = sender_type;
      }
      CORE_TRY(task, on_task_of(on, type, task_type));
      auto made = make_node<await_expr>(awaited.span, type);
      made->task = std::move(task);
      return ptr<expr>(std::move(made));
    }
    const auto is_par = kind == ast::node_kind::par_expr;
    const auto &branches =
        is_par ? ast::cast<const ast::par_expr>(operand).branches
               : ast::cast<const ast::race_expr>(operand).branches;
    auto args = ptr_vec<expr>{};
    for (const auto &branch : branches) {
      if (branch == nullptr) {
        return fail(is_par ? "par_expr" : "race_expr", "a missing branch",
                    awaited.span);
      }
      CORE_TRY(task, value_of(*branch));
      args.push_back(std::move(task));
    }
    return intrinsic_call(is_par ? "await_par" : "await_race", type,
                          std::move(args), {}, awaited.span);
  }();
}

auto body_builder::crew_of(const std::string &name,
                           const std::vector<ast::ptr<ast::node>> &body,
                           source_span span, type_id type)
    -> result<ptr<expr>> {
  const auto crew_type = checked_.types.builtin_generic("crew", {});
  const auto unit = unit_type();
  const auto has_value = type != unit;
  push_scope();
  auto made = make_node<block>(span, type);
  auto built = [&]() -> result<void> {
    auto bound = let_of(name, crew_type, false,
                        intrinsic_call("crew_new", crew_type, {}, {}, span),
                        span);
    const auto crew = bound->symbol;
    made->statements.push_back(std::move(bound));
    open_crews_.push_back(open_crew{.symbol = crew, .name = name});
    auto inner = block_of(body, span, has_value ? type : unit);
    open_crews_.pop_back();
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    auto result_place = std::optional<place>{};
    if (has_value) {
      auto kept = let_of("<crew result>", type, false,
                         ptr<expr>(std::move(*inner)), span);
      auto read = place{};
      read.root.kind = place_root::origin::local;
      read.root.symbol = kept->symbol;
      read.root.name = kept->name;
      read.root.type = type;
      result_place = std::move(read);
      made->statements.push_back(std::move(kept));
    } else {
      made->statements.push_back(
          expr_stmt_of(ptr<expr>(std::move(*inner)), span));
    }
    made->statements.push_back(
        crew_end_of(open_crew{.symbol = crew, .name = name}, span));
    if (result_place.has_value()) {
      made->result = use_of(std::move(*result_place), span);
    }
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::crew_end_of(const open_crew &crew, source_span span)
    -> ptr<stmt> {
  auto read = place{};
  read.root.kind = place_root::origin::local;
  read.root.symbol = crew.symbol;
  read.root.name = crew.name;
  read.root.type = checked_.types.builtin_generic("crew", {});
  auto args = ptr_vec<expr>{};
  args.push_back(use_of(std::move(read), span));
  return expr_stmt_of(
      intrinsic_call("crew_end", unit_type(), std::move(args), {}, span), span);
}

auto body_builder::crew_exits_into(bool loop_exit, source_span span,
                                   ptr_vec<stmt> &out) -> void {
  const auto outside = loop_exit ? crews_outside_loop_ : 0;
  for (auto i = open_crews_.size(); i > outside; --i) {
    out.push_back(crew_end_of(open_crews_[i - 1], span));
  }
}

auto body_builder::task_method(const ast::call_expr &call,
                               const ast::field_expr &field, type_id type)
    -> std::optional<result<ptr<expr>>> {
  const auto &name = field.field_name;
  if (name != "spawn" && name != "errors" && name != "get") {
    return std::nullopt;
  }
  auto object_type = type_of(*field.object);
  if (!object_type.has_value()) {
    return std::unexpected(object_type.error());
  }
  const auto &entry = checked_.types.entry(strip_refs(*object_type));
  if (entry.kind != type_kind::builtin_generic_kind ||
      (entry.name != "task" && entry.name != "crew")) {
    return std::nullopt;
  }
  const auto is_task = entry.name == "task";
  if ((is_task && name == "errors") || (!is_task && name == "get")) {
    return std::nullopt;
  }
  return [&]() -> result<ptr<expr>> {
    CORE_TRY(p, receiver_place(*field.object));
    while (is_ref(p.type())) {
      add_deref(p);
    }
    const auto param = checked_.types.ref_to(p.type(), false);
    auto args = ptr_vec<expr>{};
    args.push_back(receiver_argument(std::move(p), param, field.span));
    if (is_task && call.args.empty()) {
      return intrinsic_call(name == "spawn" ? "task_spawn" : "task_get", type,
                            std::move(args), {}, call.span);
    }
    if (!is_task && name == "spawn" && call.args.size() == 1 &&
        call.args[0].value != nullptr) {
      CORE_TRY(task, value_of(*call.args[0].value));
      args.push_back(std::move(task));
      return intrinsic_call("crew_spawn", type, std::move(args), {}, call.span);
    }
    if (!is_task && name == "errors" && call.args.empty()) {
      if (!checked_.lowering.crew_error_dispatches.contains(&call) ||
          !checked_.lowering.crew_error_task_types.contains(&call)) {
        return fail("missing record: crew_error_dispatches",
                    "`errors()` on a crew", call.span);
      }
      return intrinsic_call("crew_errors", type, std::move(args), {},
                            call.span);
    }
    return fail("task operation", std::format("`.{}()` on a {}", name,
                                              entry.name),
                call.span);
  }();
}

} // namespace cinder::core
