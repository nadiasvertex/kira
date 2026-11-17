#include <algorithm>
#include <format>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"
#include "src/parser/text_escape.h"
#include "src/util/integer_literal.h"

/// Statements, blocks, functions and statics.
namespace cinder::core {

using semantic::type_kind;

namespace {

/// Marks a block's tail `if` or `match` statement (`if_expr::tail_statement`).
auto mark_tail_statement(expr &value) -> void {
  if (value.kind == node_kind::if_expr) {
    static_cast<if_expr &>(value).tail_statement = true;
  } else if (value.kind == node_kind::match_expr) {
    static_cast<match_expr &>(value).tail_statement = true;
  }
}

[[nodiscard]] auto compound_operator(ast::assign_op op)
    -> std::optional<ast::binary_op> {
  using a = ast::assign_op;
  using b = ast::binary_op;
  switch (op) {
  case a::add_assign: return b::add;
  case a::sub_assign: return b::sub;
  case a::mul_assign: return b::mul;
  case a::div_assign: return b::div;
  case a::mod_assign: return b::mod;
  case a::and_assign: return b::bit_and;
  case a::or_assign: return b::bit_or;
  case a::xor_assign: return b::bit_xor;
  case a::shl_assign: return b::shl;
  case a::shr_assign: return b::shr;
  case a::add_wrap_assign: return b::add_wrap;
  case a::sub_wrap_assign: return b::sub_wrap;
  case a::mul_wrap_assign: return b::mul_wrap;
  case a::add_sat_assign: return b::add_sat;
  case a::sub_sat_assign: return b::sub_sat;
  case a::mul_sat_assign: return b::mul_sat;
  case a::assign: return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

auto body_builder::is_pattern_let(const ast::node &node) -> bool {
  if (node.kind != ast::node_kind::let_stmt) {
    return false;
  }
  const auto &let_ast = ast::cast<const ast::let_stmt>(node);
  return !let_ast.else_body.empty() ||
         (let_ast.pattern != nullptr &&
          let_ast.pattern->kind != ast::node_kind::binding_pattern);
}

auto body_builder::expr_stmt_of(ptr<expr> value, source_span span)
    -> ptr<stmt> {
  auto made = make_node<expr_stmt>(span);
  made->value = std::move(value);
  return made;
}

auto body_builder::block_of(const std::vector<ast::ptr<ast::node>> &stmts,
                            source_span span, type_id type)
    -> result<ptr<block>> {
  return block_from(stmts, span, type, &stmts);
}

namespace {

/// Marks the core statements `out[first..]`, made from the one AST
/// statement `node`: every one after the first continues it, and the one
/// that plays its role for drops carries it as `origin` (a jump, the
/// assignment of an `assign_stmt`, else the first).
auto stamp_statement(const ast::node &node, ptr_vec<stmt> &out,
                     std::size_t first) -> void {
  if (first >= out.size()) {
    return;
  }
  for (auto i = first + 1; i < out.size(); ++i) {
    out[i]->continues_statement = true;
  }
  const auto role = [&]() -> node_kind {
    switch (node.kind) {
    case ast::node_kind::return_stmt: return node_kind::return_stmt;
    case ast::node_kind::break_stmt: return node_kind::break_stmt;
    case ast::node_kind::continue_stmt: return node_kind::continue_stmt;
    case ast::node_kind::assign_stmt: return node_kind::assign;
    default: return node_kind::literal;
    }
  }();
  auto *carrier = out[first].get();
  if (role != node_kind::literal) {
    for (auto i = first; i < out.size(); ++i) {
      if (out[i]->kind == role) {
        carrier = out[i].get();
        break;
      }
    }
  }
  if (carrier->origin == nullptr) {
    carrier->origin = &node;
  }
}

} // namespace

auto body_builder::block_from(std::span<const ast::ptr<ast::node>> stmts,
                              source_span span, type_id type, const void *key)
    -> result<ptr<block>> {
  push_scope();
  auto made = make_node<block>(span, type);
  made->origin = key;
  const auto valued = !checked_.types.is_unit(type);
  auto last = std::optional<std::size_t>{};
  for (std::size_t i = stmts.size(); i-- > 0;) {
    if (stmts[i] != nullptr) {
      last = i;
      break;
    }
  }
  const auto built = [&]() -> result<void> {
    for (std::size_t i = 0; i < stmts.size(); ++i) {
      if (stmts[i] == nullptr) {
        continue;
      }
      const auto &node = *stmts[i];
      if (node.has_error) {
        return fail("error node", "the statement carries a parse error",
                    node.span);
      }
      if (is_pattern_let(node)) {
        const auto rest =
            stmts.subspan(i + 1);
        CORE_TRY(matched,
                 let_pattern_of(ast::cast<const ast::let_stmt>(node), rest,
                                span, type, key));
        if (valued) {
          if (matched->origin == nullptr) {
            matched->origin = &node;
          }
          made->result = std::move(matched);
        } else {
          made->statements.push_back(expr_stmt_of(std::move(matched), node.span));
          made->statements.back()->origin = &node;
        }
        return {};
      }
      if (valued && last == i) {
        if (node.kind == ast::node_kind::expr_stmt) {
          const auto &tail = ast::cast<const ast::expr_stmt>(node);
          if (tail.expr != nullptr &&
              tail.expr->kind != ast::node_kind::yield_expr) {
            CORE_TRY(value, value_as(*tail.expr, type));
            made->result = std::move(value);
            return {};
          }
        }
        if (node.kind == ast::node_kind::if_stmt) {
          const auto &tail = ast::cast<const ast::if_stmt>(node);
          if (tail.branches.empty()) {
            return fail("if_stmt", "no branches", node.span);
          }
          CORE_TRY(value,
                   if_of(tail.branches, tail.else_body, 0, node.span, type));
          value->origin = &node;
          mark_tail_statement(*value);
          made->result = std::move(value);
          return {};
        }
        if (node.kind == ast::node_kind::static_decl) {
          if (auto tail = static_tail_of(
                  ast::cast<const ast::static_decl>(node), type)) {
            CORE_TRY(value, std::move(*tail));
            made->result = std::move(value);
            return {};
          }
        }
        if (node.kind == ast::node_kind::match_stmt) {
          const auto &tail = ast::cast<const ast::match_stmt>(node);
          if (tail.subject == nullptr) {
            return fail("match_stmt", "no subject", node.span);
          }
          CORE_TRY(value, match_of(*tail.subject, tail.arms, node.span, type));
          value->origin = &node;
          mark_tail_statement(*value);
          made->result = std::move(value);
          return {};
        }
      }
      const auto first = made->statements.size();
      CORE_CHECK(stmt_into(node, made->statements));
      stamp_statement(node, made->statements, first);
    }
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return made;
}

auto body_builder::if_of(const std::vector<ast::if_branch> &branches,
                         const std::vector<ast::ptr<ast::node>> &else_body,
                         std::size_t index, source_span span, type_id type)
    -> result<ptr<expr>> {
  const auto &branch = branches[index];
  if (branch.let_pattern != nullptr) {
    return if_let_of(branches, else_body, index, span, type);
  }
  if (branch.condition == nullptr) {
    return fail("if_stmt", "a branch has no condition", branch.span);
  }
  CORE_TRY(condition, value_as(*branch.condition, checked_.types.bool_type()));
  CORE_TRY(then_branch, block_of(branch.body, branch.span, type));
  auto made = make_node<if_expr>(span, type);
  made->condition = std::move(condition);
  made->then_branch = std::move(then_branch);
  if (index + 1 < branches.size()) {
    CORE_TRY(rest, if_of(branches, else_body, index + 1, span, type));
    auto wrapper = make_node<block>(span, type);
    if (checked_.types.is_unit(type)) {
      wrapper->statements.push_back(expr_stmt_of(std::move(rest), span));
    } else {
      wrapper->result = std::move(rest);
    }
    made->else_branch = std::move(wrapper);
  } else if (!else_body.empty()) {
    CORE_TRY(fallback, block_of(else_body, span, type));
    made->else_branch = std::move(fallback);
  }
  return ptr<expr>(std::move(made));
}

namespace {

/// Whether evaluating `subscript` again gives the same value with no effect:
/// a literal or a read of a local.
[[nodiscard]] auto is_pure_subscript(const expr &subscript) -> bool {
  if (subscript.kind == node_kind::literal) {
    return true;
  }
  if (subscript.kind != node_kind::use) {
    return false;
  }
  const auto &read = static_cast<const use &>(subscript);
  return read.from.root.kind == place_root::origin::local &&
         read.from.path.empty();
}

/// A copy of `subscript`, which `is_pure_subscript`.
[[nodiscard]] auto copy_pure_subscript(const expr &subscript) -> ptr<expr> {
  if (subscript.kind == node_kind::literal) {
    const auto &lit = static_cast<const literal &>(subscript);
    auto made = make_node<literal>(lit.span, lit.type);
    made->token = lit.token;
    made->spelling = lit.spelling;
    return made;
  }
  const auto &read = static_cast<const use &>(subscript);
  auto made = make_node<use>(read.span, read.type);
  made->from.root.kind = read.from.root.kind;
  made->from.root.symbol = read.from.root.symbol;
  made->from.root.name = read.from.root.name;
  made->from.root.type = read.from.root.type;
  made->how = read.how;
  return made;
}

} // namespace

auto body_builder::bind_impure_parts(place &target, source_span span,
                                     ptr_vec<stmt> &out) -> void {
  const auto bind = [&](std::string name, ptr<expr> value) -> place {
    const auto type = value->type;
    auto bound = let_of(std::move(name), type, false, std::move(value), span);
    auto read = place{};
    read.root.kind = place_root::origin::local;
    read.root.symbol = bound->symbol;
    read.root.name = bound->name;
    read.root.type = type;
    out.push_back(std::move(bound));
    return read;
  };
  if (target.root.kind == place_root::origin::temporary) {
    auto held = bind("<compound place>", std::move(target.root.value));
    target.root = std::move(held.root);
  }
  for (auto &step : target.path) {
    if (step.kind == projection::step::element && step.subscript != nullptr &&
        !is_pure_subscript(*step.subscript)) {
      const auto subscript_span = step.subscript->span;
      auto held = bind("<compound index>", std::move(step.subscript));
      step.subscript = use_of(std::move(held), subscript_span);
    }
  }
}

auto body_builder::pure_copy(const place &p) -> place {
  auto copy = place{};
  copy.root.kind = p.root.kind;
  copy.root.symbol = p.root.symbol;
  copy.root.name = p.root.name;
  copy.root.module = p.root.module;
  copy.root.type = p.root.type;
  for (const auto &step : p.path) {
    auto next = projection{};
    next.kind = step.kind;
    next.type = step.type;
    next.name = step.name;
    next.index = step.index;
    next.in_bounds = step.in_bounds;
    if (step.subscript != nullptr) {
      next.subscript = copy_pure_subscript(*step.subscript);
    }
    copy.path.push_back(std::move(next));
  }
  return copy;
}

auto body_builder::assign_into(const ast::assign_stmt &assign,
                               ptr_vec<stmt> &out) -> result<void> {
  if (assign.target == nullptr || assign.value == nullptr) {
    return fail("assign_stmt", "no target or value", assign.span);
  }
  if (assign.target->kind == ast::node_kind::index_expr) {
    const auto &index = ast::cast<const ast::index_expr>(*assign.target);
    if (const auto found = checked_.lowering.index_set_dispatches.find(&index);
        found != checked_.lowering.index_set_dispatches.end()) {
      if (assign.op != ast::assign_op::assign) {
        return fail_feature("compound index_set",
                            "compound assignment through an `index_set` impl "
                            "is not supported yet — write `v[i] = v[i] + x`",
                            assign.span);
      }
      if (index.object == nullptr || index.index == nullptr) {
        return fail("index_expr", "no object or key", assign.span);
      }
      CORE_TRY(called,
               dispatch_call(found->second, *index.object,
                             {index.index.get(), assign.value.get()},
                             assign.span));
      out.push_back(expr_stmt_of(std::move(called), assign.span));
      return {};
    }
  }
  CORE_TRY(target, place_of(*assign.target));
  const auto target_type = target.type();
  auto made = make_node<core::assign>(assign.span);
  auto checks = ptr_vec<stmt>{};
  CORE_CHECK(field_write_checks_into(target, assign.span, checks));
  const auto finish = [&]() {
    out.push_back(std::move(made));
    for (auto &check : checks) {
      out.push_back(std::move(check));
    }
  };
  if (assign.op == ast::assign_op::assign) {
    CORE_TRY(value, value_as(*assign.value, target_type));
    made->target = std::move(target);
    made->value = std::move(value);
    finish();
    return {};
  }
  const auto op = compound_operator(assign.op);
  const auto kind = checked_.types.entry(target_type).kind;
  if (!op.has_value() ||
      (kind != type_kind::builtin_kind && kind != type_kind::ptr_kind)) {
    return fail("compound assignment",
                std::format("compound assignment to a `{}`",
                            checked_.types.display(target_type)),
                assign.span);
  }
  // `a op= b` is `a = a op b` with `a` evaluated once: each impure part of
  // its place is bound first, and both uses read the pure place left.
  const auto first = out.size();
  bind_impure_parts(target, assign.span, out);
  auto read_place = pure_copy(target);
  const auto shift = *op == ast::binary_op::shl || *op == ast::binary_op::shr;
  CORE_TRY(value_type, type_of(*assign.value));
  CORE_TRY(rhs, value_as(*assign.value,
                         shift ? strip_refs(value_type) : target_type));
  auto combined = make_node<primitive>(assign.span, target_type);
  combined->binary = *op;
  combined->operands.push_back(use_of(std::move(read_place), assign.span));
  combined->operands.push_back(std::move(rhs));
  made->target = std::move(target);
  made->value = std::move(combined);
  made->compound = true;
  finish();
  for (auto i = first + 1; i < out.size(); ++i) {
    out[i]->continues_statement = true;
  }
  return {};
}

auto body_builder::stmt_into(const ast::node &node, ptr_vec<stmt> &out)
    -> result<void> {
  if (node.has_error) {
    return fail("error node", "the statement carries a parse error", node.span);
  }
  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &let_ast = ast::cast<const ast::let_stmt>(node);
    if (let_ast.initializer == nullptr || let_ast.pattern == nullptr) {
      return fail("let_stmt", "no pattern or initializer", node.span);
    }
    if (const auto *written =
            ast::as<const ast::lambda_expr>(let_ast.initializer.get());
        written != nullptr &&
        checked_.ownership.generic_lambda_instances.contains(written)) {
      return generic_lambda_let(let_ast, *written, out);
    }
    if (is_pattern_let(node)) {
      return fail("let_stmt", "a destructuring `let` outside a block",
                  node.span);
    }
    const auto &binding =
        ast::cast<const ast::binding_pattern>(*let_ast.pattern);
    auto value = ptr<expr>{};
    auto type = k_unknown_type;
    if (checked_.node_types.contains(let_ast.pattern.get())) {
      CORE_TRY(declared, type_of(*let_ast.pattern));
      type = declared;
      CORE_TRY(init, value_as(*let_ast.initializer, type));
      value = std::move(init);
    } else {
      CORE_TRY(init, value_of(*let_ast.initializer));
      type = init->type;
      value = std::move(init);
    }
    auto made = make_node<let>(node.span);
    made->name = binding.name;
    made->type = type;
    made->is_mut = binding.is_mut;
    made->value = std::move(value);
    made->symbol = declare(binding.name, type, binding.is_mut);
    made->origin = &node;
    made->binding_origin = let_ast.pattern.get();
    out.push_back(std::move(made));
    return {};
  }
  case ast::node_kind::var_stmt: {
    const auto &var = ast::cast<const ast::var_stmt>(node);
    if (var.initializer == nullptr) {
      return fail("var_stmt", "no initializer", node.span);
    }
    auto value = ptr<expr>{};
    auto type = k_unknown_type;
    if (checked_.node_types.contains(&var)) {
      CORE_TRY(declared, type_of(var));
      type = declared;
      CORE_TRY(init, value_as(*var.initializer, type));
      value = std::move(init);
    } else {
      CORE_TRY(init, value_of(*var.initializer));
      type = init->type;
      value = std::move(init);
    }
    auto made = make_node<let>(node.span);
    made->name = var.name;
    made->type = type;
    made->is_mut = true;
    made->value = std::move(value);
    made->symbol = declare(var.name, type, true);
    made->origin = &node;
    made->binding_origin = &node;
    out.push_back(std::move(made));
    return {};
  }
  case ast::node_kind::expr_stmt: {
    const auto &statement = ast::cast<const ast::expr_stmt>(node);
    if (statement.expr == nullptr) {
      return fail("expr_stmt", "no expression", node.span);
    }
    if (const auto *yielded =
            ast::as<const ast::yield_expr>(statement.expr.get());
        yielded != nullptr && yielded->value != nullptr) {
      auto made = make_node<jump>(node_kind::yield_stmt, node.span);
      made->suspension = yielded;
      CORE_TRY(value, value_as(*yielded->value, yield_type_));
      made->value = std::move(value);
      out.push_back(std::move(made));
      return {};
    }
    if (statement.expr->kind == ast::node_kind::yield_expr) {
      return fail_feature("yield_expr", "`yield` with no value is not supported",
                          node.span);
    }
    CORE_TRY(value, value_of(*statement.expr));
    out.push_back(expr_stmt_of(std::move(value), node.span));
    return {};
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = ast::cast<const ast::return_stmt>(node);
    auto value = ptr<expr>{};
    if (ret.value != nullptr) {
      CORE_TRY(returned, value_as(*ret.value, body_type_));
      value = std::move(returned);
    }
    CORE_TRY(made, return_of(std::move(value), node.span, &node));
    out.push_back(std::move(made));
    return {};
  }
  case ast::node_kind::break_stmt:
    crew_exits_into(true, node.span, out);
    out.push_back(make_node<jump>(node_kind::break_stmt, node.span));
    out.back()->origin = &node;
    return {};
  case ast::node_kind::continue_stmt:
    crew_exits_into(true, node.span, out);
    out.push_back(make_node<jump>(node_kind::continue_stmt, node.span));
    out.back()->origin = &node;
    return {};
  case ast::node_kind::crew_stmt: {
    const auto &crew = ast::cast<const ast::crew_stmt>(node);
    CORE_TRY(made, crew_of(&node, crew.name, crew.body, node.span, unit_type()));
    out.push_back(expr_stmt_of(std::move(made), node.span));
    return {};
  }
  case ast::node_kind::if_stmt: {
    const auto &if_ast = ast::cast<const ast::if_stmt>(node);
    if (if_ast.branches.empty()) {
      return fail("if_stmt", "no branches", node.span);
    }
    CORE_TRY(value,
             if_of(if_ast.branches, if_ast.else_body, 0, node.span, unit_type()));
    out.push_back(expr_stmt_of(std::move(value), node.span));
    return {};
  }
  case ast::node_kind::match_stmt: {
    const auto &match_ast = ast::cast<const ast::match_stmt>(node);
    if (match_ast.subject == nullptr) {
      return fail("match_stmt", "no subject", node.span);
    }
    CORE_TRY(value,
             match_of(*match_ast.subject, match_ast.arms, node.span, unit_type()));
    out.push_back(expr_stmt_of(std::move(value), node.span));
    return {};
  }
  case ast::node_kind::assign_stmt:
    return assign_into(ast::cast<const ast::assign_stmt>(node), out);
  case ast::node_kind::while_stmt: {
    const auto &loop = ast::cast<const ast::while_stmt>(node);
    if (loop.let_pattern != nullptr || loop.let_expr != nullptr) {
      if (loop.let_pattern == nullptr || loop.let_expr == nullptr) {
        return fail("while_stmt", "`while let` is incomplete", node.span);
      }
      const auto outside =
          std::exchange(crews_outside_loop_, open_crews_.size());
      auto made = while_let_of(loop);
      crews_outside_loop_ = outside;
      if (!made.has_value()) {
        return std::unexpected(made.error());
      }
      out.push_back(std::move(*made));
      return {};
    }
    if (loop.condition == nullptr) {
      return fail("while_stmt", "no condition", node.span);
    }
    CORE_TRY(condition, value_as(*loop.condition, checked_.types.bool_type()));
    const auto outside =
        std::exchange(crews_outside_loop_, open_crews_.size());
    auto body = block_of(loop.body, node.span, unit_type());
    crews_outside_loop_ = outside;
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    auto made = make_node<while_loop>(node.span);
    made->condition = std::move(condition);
    made->body = std::move(*body);
    out.push_back(std::move(made));
    return {};
  }
  case ast::node_kind::for_stmt: {
    const auto outside =
        std::exchange(crews_outside_loop_, open_crews_.size());
    auto made = for_of(ast::cast<const ast::for_stmt>(node));
    crews_outside_loop_ = outside;
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    out.push_back(std::move(*made));
    return {};
  }
  case ast::node_kind::scope_stmt: {
    const auto &scope = ast::cast<const ast::scope_stmt>(node);
    CORE_TRY(body, block_of(scope.body, node.span, unit_type()));
    out.push_back(expr_stmt_of(std::move(body), node.span));
    return {};
  }
  case ast::node_kind::splice_stmt: {
    const auto found = checked_.spliced_fragments.find(&node);
    if (found == checked_.spliced_fragments.end() || found->second == nullptr) {
      return fail("missing record: spliced_fragments", "an unresolved splice",
                  node.span);
    }
    if (const auto *fragment = ast::as<const ast::expr>(found->second)) {
      CORE_TRY(value, value_of(*fragment));
      out.push_back(expr_stmt_of(std::move(value), node.span));
      return {};
    }
    if (is_pattern_let(*found->second)) {
      return fail("let_stmt", "a spliced destructuring `let`", node.span);
    }
    return stmt_into(*found->second, out);
  }
  case ast::node_kind::static_decl:
    return static_decl_into(ast::cast<const ast::static_decl>(node), out);
  default:
    return fail_feature(kind_name(node.kind),
                        std::format("statement kind {} is not lowered yet "
                                    "(see spec/todo.md)",
                                    static_cast<int>(node.kind)),
                        node.span);
  }
}

// ==========================================================================
//  Functions and statics
// ==========================================================================

auto body_builder::function_of(const ast::func_decl &decl, std::string name,
                               std::string module) -> result<ptr<function>> {
  if (decl.has_error) {
    return fail("error node", "the declaration carries a parse error",
                decl.span);
  }
  CORE_TRY(sig, signature_of(decl));
  post_contracts_.clear();
  for (const auto &contract : decl.contracts) {
    if (!contract_checks_) {
      break;
    }
    if (contract.condition == nullptr ||
        checked_.lowering.elided_contracts.contains(&contract)) {
      continue;
    }
    // A generator's `pre` is checked once, at the top of its body, before
    // the check that it was dropped before it first ran.
    if (sig.coroutine == coroutine_kind::task ||
        (sig.coroutine == coroutine_kind::generator && !contract.is_pre)) {
      return fail_feature(
          "contract",
          sig.coroutine == coroutine_kind::task
              ? std::format("`async def {}` needs a declared return type, "
                            "and cannot be a generator or have a `post` "
                            "condition yet",
                            decl.name)
              : std::format("the generator `{}` cannot have a `post` "
                            "condition yet",
                            decl.name),
          contract.span);
    }
    if (!contract.is_pre) {
      post_contracts_.push_back(&contract);
    }
  }
  auto made = make_node<function>();
  made->name = std::move(name);
  made->module = std::move(module);
  module_name_ = made->module;
  decl_ = &decl;
  made->span = decl.span;
  made->declared_name = decl.name;
  made->decl = &decl;
  made->expression_body = decl.body_expr != nullptr;
  if (sig.coroutine != coroutine_kind::none) {
    made->start = semantic::ownership::generator_start(decl);
  }
  made->return_type = sig.result;
  made->body_type = sig.body;
  made->yield_type = sig.yield_type;
  made->coroutine = sig.coroutine;
  made->receiver = sig.receiver;
  if (sig.receiver == receiver_mode::consuming && !decl.params.empty()) {
    const auto found = checked_.node_types.find(decl.params[0].pattern.get());
    if (found != checked_.node_types.end()) {
      made->receiver_type = found->second;
    }
  }
  made->generic_params = generic_params_;
  // A module-level `static def` (or an instance of one); `is_static` alone
  // also marks a static method, which runs like any other function.
  made->compile_time =
      compile_time_ ||
      checked_.lowering.comptime_only_functions.contains(&decl);
  body_type_ = sig.body;
  yield_type_ = sig.yield_type;
  push_scope();
  // A destructuring parameter is a parameter of its own, named `<param i>`
  // as the old lowerer names it, whose pattern binds at the top of the body.
  auto destructured = std::vector<std::size_t>{};
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    const auto *pattern = decl.params[i].pattern.get();
    const auto *binding = ast::as<const ast::binding_pattern>(pattern);
    const auto name =
        binding != nullptr ? binding->name : std::format("<param {}>", i);
    if (binding == nullptr) {
      destructured.push_back(i);
    }
    made->params.push_back(param{.symbol = declare(name, sig.params[i], true),
                                 .name = name,
                                 .type = sig.params[i],
                                 .origin = pattern,
                                 .span = decl.params[i].span});
  }
  const auto plain_body = [&]() -> result<ptr<block>> {
    auto built = [&]() -> result<ptr<block>> {
      if (decl.body_expr != nullptr) {
        CORE_TRY(value, value_as(*decl.body_expr, sig.body));
        auto wrapper = make_node<block>(decl.body_expr->span, sig.body);
        wrapper->result = std::move(value);
        return wrapper;
      }
      return block_of(decl.body_stmts, decl.span, sig.body);
    }();
    if (built.has_value()) {
      if (auto checked = contracts_into(decl, **built); !checked.has_value()) {
        return std::unexpected(checked.error());
      }
    }
    return built;
  };
  auto patterns = std::vector<const ast::node *>{};
  for (const auto &written : decl.params) {
    patterns.push_back(written.pattern.get());
  }
  auto body = destructured_body(made->params, patterns, destructured,
                                sig.body, plain_body);
  pop_scope();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  made->body = std::move(*body);
  return made;
}

auto body_builder::destructured_body(
    const std::vector<param> &params, const std::vector<const ast::node *> &written,
    std::span<const std::size_t> destructured, type_id body_type,
    const std::function<result<ptr<block>>()> &body) -> result<ptr<block>> {
  if (destructured.empty()) {
    return body();
  }
  // The first is a pattern `let` of the parameter whose arm holds the rest
  // of the body: the next destructuring parameter's, then the body.
  const auto i = destructured.front();
  const auto &pattern = *written[i];
  const auto span = pattern.span;
  auto subject = place{};
  subject.root.kind = place_root::origin::local;
  subject.root.symbol = params[i].symbol;
  subject.root.name = params[i].name;
  subject.root.type = params[i].type;
  auto context = binding_context{};
  context.subject_access = access_of(subject);
  context.subject_type = subject.type();
  CORE_TRY(alternative, alternative_of(pattern, subject.type(), context));
  auto alternatives = std::vector<pattern_alternative>{};
  alternatives.push_back(std::move(alternative));
  CORE_TRY(arm, arm_of(
                    std::move(alternatives), nullptr,
                    [&]() {
                      return destructured_body(params, written,
                                               destructured.subspan(1),
                                               body_type, body);
                    },
                    span, &pattern));
  auto matched = make_node<match_expr>(span, body_type);
  matched->subject = std::move(subject);
  matched->form = match_form::let_pattern;
  // The rest of the body is still the body's scope.
  auto outer = make_node<block>(arm.body->span, body_type);
  outer->origin = arm.body->origin;
  matched->arms.push_back(std::move(arm));
  if (checked_.types.is_unit(body_type)) {
    outer->statements.push_back(expr_stmt_of(std::move(matched), span));
  } else {
    outer->result = std::move(matched);
  }
  return outer;
}

auto body_builder::reified(const semantic::reified_value &value,
                           source_span span) -> result<ptr<expr>> {
  using shape = semantic::reified_value::shape;
  CORE_TRY(type, settle(value.type, span));
  auto parts = ptr_vec<expr>{};
  for (const auto &part : value.parts) {
    CORE_TRY(built, reified(part, span));
    parts.push_back(std::move(built));
  }
  switch (value.kind) {
  case shape::scalar: {
    auto lit = make_node<literal>(span, type);
    switch (value.leaf.kind) {
    case comptime::value_kind::integer: {
      const auto is_signed = semantic::is_signed_integer_name(
          checked_.types.entry(type).name);
      lit->token = token_kind::int_lit;
      lit->spelling =
          is_signed ? util::format_wide_signed_decimal(value.leaf.integer)
                    : util::format_wide_decimal(
                          static_cast<unsigned __int128>(value.leaf.integer));
      break;
    }
    case comptime::value_kind::floating:
      lit->token = token_kind::float_lit;
      lit->spelling = std::to_string(value.leaf.floating);
      break;
    case comptime::value_kind::boolean:
      lit->token = value.leaf.boolean ? token_kind::kw_true : token_kind::kw_false;
      lit->spelling = value.leaf.boolean ? "true" : "false";
      break;
    default:
      lit->token = token_kind::string_lit;
      lit->spelling = encode_string_literal(value.leaf.string);
      break;
    }
    return ptr<expr>(std::move(lit));
  }
  case shape::array: {
    auto made = make_node<aggregate_init>(node_kind::array_init, span, type);
    made->elements = std::move(parts);
    return ptr<expr>(std::move(made));
  }
  case shape::tuple: {
    auto made = make_node<aggregate_init>(node_kind::tuple_init, span, type);
    made->elements = std::move(parts);
    return ptr<expr>(std::move(made));
  }
  case shape::structure: {
    auto made = make_node<aggregate_init>(node_kind::struct_init, span, type);
    made->elements = std::move(parts);
    made->field_names = value.names;
    const auto *shape = find_shape(checked_, type);
    if (shape == nullptr) {
      return fail("missing record: shapes",
                  std::format("no shape for `{}`", checked_.types.display(type)),
                  span);
    }
    for (const auto &name : value.names) {
      const auto &fields = shape->fields;
      const auto field =
          std::ranges::find(fields, name, &semantic::field_shape::name);
      if (field == fields.end()) {
        return fail("missing record: shapes",
                    std::format("`{}` has no field `{}`",
                                checked_.types.display(type), name),
                    span);
      }
      made->field_indices.push_back(
          static_cast<std::size_t>(field - fields.begin()));
    }
    return ptr<expr>(std::move(made));
  }
  case shape::variant: {
    if (value.names.empty()) {
      return fail("missing record: static_global_defs",
                  "a reified variant with no name", span);
    }
    CORE_TRY(index, variant_index(type, value.names.front(), span));
    auto made = make_node<variant_init>(span, type);
    made->variant = value.names.front();
    made->variant_index = index;
    made->payload = std::move(parts);
    return ptr<expr>(std::move(made));
  }
  case shape::collection:
    break;
  }
  const auto conversion =
      checked_.lowering.array_literal_conversions.find(value.conversion);
  if (conversion == checked_.lowering.array_literal_conversions.end()) {
    return fail("missing record: array_literal_conversions",
                "a reified collection with no `from_array`", span);
  }
  CORE_TRY(array_type, settle(conversion->second.array_type, span));
  CORE_TRY(target, callee_of(conversion->second.callee, span));
  const auto params = checked_.types.entry(target.signature).args;
  if (params.size() != 1) {
    return fail("missing record: array_literal_conversions",
                "the conversion does not take one parameter", span);
  }
  auto inner = make_node<aggregate_init>(node_kind::array_init, span, array_type);
  inner->elements = std::move(parts);
  auto args = ptr_vec<expr>{};
  CORE_TRY(converted, coerce(std::move(inner), params[0]));
  args.push_back(std::move(converted));
  return direct_call(std::move(target), std::move(args), span);
}

auto build_function(semantic::checked_types &checked,
                    const function_lookup &functions,
                    const template_table &templates,
                    const ast::func_decl &decl, std::string name,
                    std::string module, std::vector<type_id> generic_params,
                    const adapt_options &options) -> result<ptr<function>> {
  auto builder = body_builder(checked, &functions);
  builder.contract_checks_ = options.contract_checks;
  builder.compile_time_ = options.compile_time;
  builder.open_generic_params(std::move(generic_params), &templates);
  return builder.function_of(decl, std::move(name), std::move(module));
}

auto build_expression_function(semantic::checked_types &checked,
                               const function_lookup &functions,
                               const ast::expr &expr,
                               std::span<const expression_capture> captures,
                               std::string name, std::string module)
    -> result<ptr<function>> {
  auto builder = body_builder(checked, &functions);
  builder.compile_time_ = true;
  builder.fresh_static_reads_ = true;
  // Nothing it owns is dropped and nothing it reads is borrowed by a
  // closure the ownership check knows of: it has no declaration.
  builder.owned_static_reads_.emplace();
  builder.borrowed_captures_.emplace();
  builder.task_captures_.emplace();
  CORE_TRY(type, builder.type_of(expr));
  auto made = make_node<function>();
  made->name = std::move(name);
  made->module = std::move(module);
  builder.module_name_ = made->module;
  made->span = expr.span;
  made->declared_name = made->name;
  made->return_type = type;
  made->body_type = type;
  made->compile_time = true;
  builder.body_type_ = type;
  builder.push_scope();
  for (const auto &capture : captures) {
    CORE_TRY(param_type, builder.settle(capture.type, expr.span));
    made->params.push_back(
        param{.symbol = builder.declare(capture.name, param_type, true),
              .name = capture.name,
              .type = param_type,
              .origin = nullptr,
              .span = expr.span});
  }
  auto value = builder.value_as(expr, type);
  builder.pop_scope();
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  auto body = make_node<block>(expr.span, type);
  body->result = std::move(*value);
  made->body = std::move(body);
  return made;
}

auto build_intrinsic_signature(semantic::checked_types &checked,
                               const ast::func_decl &decl,
                               std::vector<type_id> generic_params)
    -> result<type_id> {
  auto builder = body_builder(checked);
  builder.open_generic_params(std::move(generic_params), nullptr);
  CORE_TRY(sig, builder.signature_of(decl));
  return sig.fn_type;
}

auto build_static_value(semantic::checked_types &checked,
                        const semantic::reified_value &value, source_span span)
    -> result<ptr<expr>> {
  auto builder = body_builder(checked);
  return builder.reified(value, span);
}

} // namespace cinder::core
