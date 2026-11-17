// Lambdas and their captures (spec/unified-checker-plan.md, phase 2).
//
// A lambda's body is built in scopes of its own. A name it uses that is
// declared outside it becomes a capture the first time it is used: a fresh
// symbol inside the body, with `from` naming the outer place (rule E10).
// How each capture holds its value comes from the written capture list,
// or, without one, from the ownership checker's record of what the closure
// borrows; everything else is copied when `copy` and moved otherwise, as
// lowering captures it.

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

namespace cinder::core {

using semantic::type_kind;

auto body_builder::load_capture_records() -> void {
  if (borrowed_captures_.has_value()) {
    return;
  }
  auto records = borrowed_records{};
  auto tasks = task_records{};
  auto owned_reads = std::unordered_set<const void *>{};
  if (decl_ != nullptr) {
    for (auto &cfg :
         semantic::ownership::build_function_cfgs(*decl_, checked_)) {
      for (auto &[lambda, list] : cfg.borrowed_captures) {
        records.insert_or_assign(lambda, std::move(list));
      }
      for (auto &[block, list] : cfg.task_captures) {
        tasks.insert_or_assign(block, std::move(list));
      }
      owned_reads.insert(cfg.owned_static_reads.begin(),
                         cfg.owned_static_reads.end());
    }
  }
  borrowed_captures_ = std::move(records);
  task_captures_ = std::move(tasks);
  owned_static_reads_ = std::move(owned_reads);
}

auto body_builder::owned_static_reads()
    -> const std::unordered_set<const void *> & {
  load_capture_records();
  return *owned_static_reads_;
}

auto body_builder::borrowed_captures() -> const borrowed_records & {
  load_capture_records();
  return *borrowed_captures_;
}

auto body_builder::task_captures() -> const task_records & {
  load_capture_records();
  return *task_captures_;
}

auto body_builder::capture_mode_of(const lambda_frame &frame,
                                   const std::string &name, type_id type)
    -> capture::mode {
  const auto base = name.substr(0, name.find('#'));
  if (frame.source == nullptr) {
    // A task block: the ownership checker's `task_capture` records which
    // locals the task owns (moved in); the others are lent to it.
    const auto &records = task_captures();
    if (const auto found = records.find(frame.task_key);
        found != records.end()) {
      const auto entry = std::ranges::find(
          found->second, base, &semantic::ownership::task_capture::name);
      if (entry != found->second.end() && entry->moved) {
        return checked_.is_copy(type) ? capture::mode::copy
                                      : capture::mode::move;
      }
    }
    return checked_.is_copy(type) ? capture::mode::copy
                                  : capture::mode::borrow;
  }
  const auto &lambda = *frame.source;
  if (lambda.captures.has_value()) {
    for (const auto &entry : *lambda.captures) {
      if (entry.name != base) {
        continue;
      }
      switch (entry.mode) {
      case ast::capture_mode::by_ref:
        return capture::mode::borrow;
      case ast::capture_mode::by_mut_ref:
        return capture::mode::borrow_mut;
      case ast::capture_mode::by_move:
        return capture::mode::move;
      case ast::capture_mode::by_value:
        break;
      }
    }
  } else if (!lambda.is_move) {
    // The ownership checker's `borrow_capture`: a non-`copy` local the body
    // does not consume is borrowed, mutably when the body changes it.
    const auto &records = borrowed_captures();
    if (const auto found = records.find(&lambda); found != records.end()) {
      const auto entry =
          std::ranges::find(found->second, base,
                            &semantic::ownership::borrowed_capture::name);
      if (entry != found->second.end()) {
        return entry->is_mut ? capture::mode::borrow_mut
                             : capture::mode::borrow;
      }
    }
  }
  // A `copy` value moved is copied.
  return checked_.is_copy(type) ? capture::mode::copy : capture::mode::move;
}

auto body_builder::capture_into(lambda_frame &frame, const std::string &name,
                                const local_info &outer) -> local_info {
  // A task block's name with no `task_capture` record is lent, as lowering
  // treats it: the records only decide which captures move in. (A name a
  // nested task uses has no record on the enclosing task.)
  if (frame.source != nullptr && frame.source->captures.has_value() &&
      !frame.unlisted.has_value()) {
    const auto base = name.substr(0, name.find('#'));
    const auto listed = std::ranges::any_of(
        *frame.source->captures,
        [&](const ast::lambda_capture &entry) { return entry.name == base; });
    if (!listed) {
      frame.unlisted = name;
    }
  }
  const auto how = capture_mode_of(frame, name, outer.type);
  const auto borrowed =
      how == capture::mode::borrow || how == capture::mode::borrow_mut;
  auto made = capture{};
  made.symbol = mint();
  made.how = how;
  made.type = outer.type;
  made.from.root.kind = place_root::origin::local;
  made.from.root.symbol = outer.symbol;
  made.from.root.name = name;
  made.from.root.type = outer.type;
  const auto info = local_info{
      .symbol = made.symbol,
      .type = outer.type,
      .writable = how == capture::mode::move ||
                  how == capture::mode::borrow_mut,
      .pinned = borrowed};
  scopes_[frame.scope_base].insert_or_assign(name, info);
  frame.captures.push_back(std::move(made));
  return info;
}

auto body_builder::lambda_of(const ast::lambda_expr &lambda)
    -> result<ptr<expr>> {
  CORE_TRY(type, type_of(lambda));
  const auto &entry = checked_.types.entry(type);
  if (entry.kind != type_kind::fn_kind) {
    return fail("lambda_expr",
                std::format("a lambda typed `{}`, which is not `fn`",
                            checked_.types.display(type)),
                lambda.span);
  }
  if (entry.args.size() != lambda.params.size()) {
    return fail("lambda_expr", "a lambda whose parameters do not match its type",
                lambda.span);
  }
  const auto params = entry.args;
  const auto return_type = entry.result;
  auto made = make_node<core::lambda>(lambda.span, type);
  made->return_type = return_type;
  made->capture_list = lambda.captures.has_value();
  made->closure_origin = &lambda;

  // A written list's entries are captured where the lambda is made, in
  // list order, even when the body never uses one.
  push_scope();
  lambdas_.push_back(lambda_frame{.source = &lambda,
                                  .task_key = nullptr,
                                  .scope_base = scopes_.size() - 1,
                                  .captures = {},
                                  .unlisted = std::nullopt});
  const auto saved_body_type = std::exchange(body_type_, return_type);
  auto saved_posts = std::exchange(post_contracts_, {});
  auto saved_crews = std::exchange(open_crews_, {});
  const auto saved_outside = std::exchange(crews_outside_loop_, 0);
  const auto finish = [&]() -> lambda_frame {
    body_type_ = saved_body_type;
    post_contracts_ = std::move(saved_posts);
    open_crews_ = std::move(saved_crews);
    crews_outside_loop_ = saved_outside;
    auto frame = std::move(lambdas_.back());
    lambdas_.pop_back();
    while (scopes_.size() > frame.scope_base) {
      pop_scope();
    }
    return frame;
  };
  if (lambda.captures.has_value()) {
    for (const auto &item : *lambda.captures) {
      // `[f]` of a generic lambda captures every copy made for `f`.
      if (const auto group = generic_lambda_names_.find(item.name);
          group != generic_lambda_names_.end() &&
          !lookup(item.name).has_value()) {
        const auto names = group->second;
        for (const auto &copy : names) {
          static_cast<void>(lookup(copy));
        }
        continue;
      }
      if (!lookup(item.name).has_value()) {
        static_cast<void>(finish());
        return fail("lambda_expr",
                    std::format("the capture `{}` names no local", item.name),
                    item.span);
      }
    }
  }
  push_scope();
  // A destructuring parameter is a parameter named `<param i>`, as the old
  // lowerer names it, whose pattern binds at the top of the body.
  auto destructured = std::vector<std::size_t>{};
  auto patterns = std::vector<const ast::node *>{};
  for (std::size_t i = 0; i < lambda.params.size(); ++i) {
    const auto *pattern = lambda.params[i].pattern.get();
    const auto *binding = ast::as<const ast::binding_pattern>(pattern);
    const auto name =
        binding != nullptr ? binding->name : std::format("<param {}>", i);
    if (binding == nullptr) {
      destructured.push_back(i);
    }
    patterns.push_back(pattern);
    made->params.push_back(param{.symbol = declare(name, params[i], true),
                                 .name = name,
                                 .type = params[i],
                                 .origin = pattern});
  }
  const auto plain_body = [&]() -> result<ptr<block>> {
    if (lambda.body_expr != nullptr) {
      CORE_TRY(value, value_as(*lambda.body_expr, return_type));
      auto wrapper = make_node<block>(lambda.body_expr->span, return_type);
      wrapper->result = std::move(value);
      return wrapper;
    }
    return block_of(lambda.body_stmts, lambda.span, return_type);
  };
  auto body = destructured_body(made->params, patterns, destructured,
                                return_type, plain_body);
  auto frame = finish();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  if (frame.unlisted.has_value()) {
    return fail("missing record: capture list",
                std::format("the body uses `{}`, which its capture list does "
                            "not hold",
                            *frame.unlisted),
                lambda.span);
  }
  made->captures = std::move(frame.captures);
  made->body = std::move(*body);
  return ptr<expr>(std::move(made));
}

auto body_builder::generic_lambda_let(const ast::let_stmt &let,
                                      const ast::lambda_expr &written,
                                      ptr_vec<stmt> &out) -> result<void> {
  // The written lambda has no run-time form; each copy the checker made for
  // a use is a closure of its own, bound to a local of its own (lowering's
  // `generic_lambda_groups_`).
  const auto *binding = ast::as<const ast::binding_pattern>(let.pattern.get());
  if (binding == nullptr) {
    return fail("generic lambda", "a generic lambda bound by a pattern",
                let.span);
  }
  auto names = std::vector<std::string>{};
  for (const auto *copy :
       checked_.ownership.generic_lambda_instances.at(&written)) {
    CORE_TRY(closure, lambda_of(*copy));
    auto name = copy_name(binding->name, *copy);
    const auto type = closure->type;
    auto bound = make_node<core::let>(let.span);
    bound->name = name;
    bound->type = type;
    bound->value = std::move(closure);
    bound->symbol = declare(name, type);
    // The ownership check declares each copy as a binding of its own,
    // named by the copy.
    bound->binding_origin = copy;
    names.push_back(std::move(name));
    out.push_back(std::move(bound));
  }
  generic_lambda_names_.insert_or_assign(binding->name, std::move(names));
  return {};
}

auto body_builder::copy_name(const std::string &name,
                             const ast::lambda_expr &copy) -> std::string {
  return std::format("{}#{}", name, static_cast<const void *>(&copy));
}

} // namespace cinder::core
