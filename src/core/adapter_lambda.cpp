// Lambdas and their captures (spec/unified-checker-plan.md, phase 2).
//
// A lambda's body is built in scopes of its own. A name it uses that is
// declared outside it becomes a capture the first time it is used: a fresh
// symbol inside the body, with `from` naming the outer place (rule E10).
// How each capture holds its value comes from the written capture list.
// Without one, a `copy` value is copied, and any other is borrowed unless
// the body consumes it, mutably when the body changes it; that is settled
// from the built body (`settle_captures`). A `move` lambda moves each.

#include <algorithm>
#include <unordered_set>
#include <format>
#include <string>
#include <utility>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

namespace cinder::core {

using semantic::type_kind;

auto body_builder::capture_mode_of(const lambda_frame &frame,
                                   const std::string &name, type_id type)
    -> capture::mode {
  const auto base = name.substr(0, name.find('#'));
  if (frame.source != nullptr && frame.source->captures.has_value()) {
    for (const auto &entry : *frame.source->captures) {
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
  }
  // A `copy` value is copied. Any other is moved for now: a task block, or
  // a lambda that is not `move`, settles it once its body is built
  // (`settle_captures`).
  return checked_.is_copy(type) ? capture::mode::copy : capture::mode::move;
}

namespace {

/// What a body does with one capture of the lambda or task being settled:
/// whether it consumes it (a move of it or a part of it, a binding that
/// moves out of it, a nested closure that moves it in) or changes it (an
/// assignment, a `&mut`, a nested closure that borrows it mutably).
struct capture_effects {
  bool consumed = false;
  bool written = false;
};

class capture_scan {
public:
  explicit capture_scan(symbol_id capture) { names_.insert(capture); }

  auto scan(block &b) -> void { block_of(b); }

  [[nodiscard]] auto effects() const -> capture_effects { return effects_; }

  /// The `alias` bindings rooted at the capture: what a downgrade to a
  /// shared borrow must make read-only.
  [[nodiscard]] auto aliases() const -> const std::vector<pattern_binding *> & {
    return aliases_;
  }

private:
  std::unordered_set<symbol_id> names_;
  capture_effects effects_;
  std::vector<pattern_binding *> aliases_;

  [[nodiscard]] auto rooted(const place &p) const -> bool {
    return p.root.kind == place_root::origin::local &&
           names_.contains(p.root.symbol);
  }

  auto place_parts(place &p) -> void {
    if (p.root.value != nullptr) {
      expr_of(*p.root.value);
    }
    for (auto &step : p.path) {
      if (step.subscript != nullptr) {
        expr_of(*step.subscript);
      }
    }
  }

  auto block_of(block &b) -> void {
    for (auto &s : b.statements) {
      stmt_of(*s);
    }
    if (b.result != nullptr) {
      expr_of(*b.result);
    }
  }

  auto stmt_of(stmt &s) -> void {
    switch (s.kind) {
    case node_kind::let:
      if (auto &value = static_cast<let &>(s).value; value != nullptr) {
        expr_of(*value);
      }
      return;
    case node_kind::assign: {
      auto &a = static_cast<assign &>(s);
      if (rooted(a.target)) {
        effects_.written = true;
      }
      place_parts(a.target);
      if (a.value != nullptr) {
        expr_of(*a.value);
      }
      return;
    }
    case node_kind::expr_stmt:
      expr_of(*static_cast<expr_stmt &>(s).value);
      return;
    case node_kind::while_loop: {
      auto &loop = static_cast<while_loop &>(s);
      if (loop.condition != nullptr) {
        expr_of(*loop.condition);
      }
      block_of(*loop.body);
      if (loop.step != nullptr) {
        block_of(*loop.step);
      }
      return;
    }
    case node_kind::return_stmt:
    case node_kind::yield_stmt:
      if (auto &value = static_cast<jump &>(s).value; value != nullptr) {
        expr_of(*value);
      }
      return;
    case node_kind::contract_check:
      if (auto &c = static_cast<contract_check &>(s).condition; c != nullptr) {
        expr_of(*c);
      }
      return;
    default:
      return;
    }
  }

  auto each(ptr_vec<expr> &list) -> void {
    for (auto &e : list) {
      expr_of(*e);
    }
  }

  auto expr_of(expr &e) -> void {
    switch (e.kind) {
    case node_kind::use: {
      auto &u = static_cast<use &>(e);
      if (u.how == use::mode::move && rooted(u.from)) {
        effects_.consumed = true;
      }
      place_parts(u.from);
      return;
    }
    case node_kind::borrow: {
      auto &b = static_cast<borrow &>(e);
      if (b.is_mut && rooted(b.of)) {
        effects_.written = true;
      }
      place_parts(b.of);
      return;
    }
    case node_kind::call: {
      auto &c = static_cast<call &>(e);
      if (c.target.value != nullptr) {
        expr_of(*c.target.value);
      }
      each(c.args);
      return;
    }
    case node_kind::primitive:
      each(static_cast<primitive &>(e).operands);
      return;
    case node_kind::cast:
      if (auto &o = static_cast<cast &>(e).operand; o != nullptr) {
        expr_of(*o);
      }
      return;
    case node_kind::tuple_init:
    case node_kind::array_init:
    case node_kind::struct_init:
      each(static_cast<aggregate_init &>(e).elements);
      return;
    case node_kind::variant_init:
      each(static_cast<variant_init &>(e).payload);
      return;
    case node_kind::lambda:
      // The nested body names its own captures; what it takes from here is
      // in its capture list.
      for (auto &c : static_cast<lambda &>(e).captures) {
        if (rooted(c.from)) {
          effects_.consumed =
              effects_.consumed || c.how == capture::mode::move;
          effects_.written =
              effects_.written || c.how == capture::mode::borrow_mut;
        }
        place_parts(c.from);
      }
      return;
    case node_kind::block:
      block_of(static_cast<block &>(e));
      return;
    case node_kind::if_expr: {
      auto &i = static_cast<if_expr &>(e);
      if (i.condition != nullptr) {
        expr_of(*i.condition);
      }
      block_of(*i.then_branch);
      if (i.else_branch != nullptr) {
        block_of(*i.else_branch);
      }
      return;
    }
    case node_kind::match_expr: {
      auto &m = static_cast<match_expr &>(e);
      const auto subject_rooted = rooted(m.subject);
      place_parts(m.subject);
      for (auto &arm : m.arms) {
        for (auto &alt : arm.alternatives) {
          for (auto &b : alt.bindings) {
            if (!subject_rooted) {
              continue;
            }
            if (b.how == pattern_binding::mode::move) {
              effects_.consumed = true;
            } else if (b.how == pattern_binding::mode::alias) {
              names_.insert(b.symbol);
              aliases_.push_back(&b);
            }
          }
        }
        if (arm.guard != nullptr) {
          expr_of(*arm.guard);
        }
        block_of(*arm.body);
      }
      return;
    }
    case node_kind::await_expr:
      if (auto &t = static_cast<await_expr &>(e).task; t != nullptr) {
        expr_of(*t);
      }
      return;
    case node_kind::host_call:
      each(static_cast<host_call &>(e).args);
      return;
    default:
      return;
    }
  }
};

} // namespace

auto body_builder::settle_captures(lambda &made, bool task) const -> void {
  for (auto &c : made.captures) {
    if (c.how != capture::mode::move || checked_.is_copy(c.type)) {
      continue;
    }
    auto scan = capture_scan(c.symbol);
    scan.scan(*made.body);
    const auto effects = scan.effects();
    // A task's body runs once, so a capture it consumes moves in; a task
    // borrows any other (a write to one is the ownership check's to
    // report). A lambda borrows it, mutably when the body changes it
    // (ch. 16): a closure can be called again, so its body may never move a
    // capture out, and the ownership check reports one that tries.
    // A task that writes through a captured `&mut` takes the reference:
    // borrowing it would leave the owner reading what the task writes.
    const auto mut_ref =
        !checked_.types.is_unknown(c.type) &&
        checked_.types.entry(c.type).kind == semantic::type_kind::ref_kind &&
        checked_.types.entry(c.type).is_mut;
    if (task && (effects.consumed || (effects.written && mut_ref))) {
      continue;
    }
    c.how = effects.written && !task ? capture::mode::borrow_mut
                                     : capture::mode::borrow;
    if (c.how == capture::mode::borrow && !effects.written) {
      for (auto *alias : scan.aliases()) {
        alias->writable = false;
      }
    }
  }
}

auto body_builder::capture_into(lambda_frame &frame, const std::string &name,
                                const local_info &outer) -> local_info {
  // A name a written capture list does not hold is reported.
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
                                 .origin = pattern,
                                 .span = pattern != nullptr
                                             ? pattern->span
                                             : lambda.span});
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
  // An implicit capture, or a bare `name` in a capture list, follows the
  // borrow-or-move rule unless the lambda is `move` (ch. 16).
  if (!lambda.is_move) {
    settle_captures(*made, /*task=*/false);
  }
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
