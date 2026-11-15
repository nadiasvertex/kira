// `static if` and `static for` in function bodies (spec/unified-checker-plan.md,
// phase 2).
//
// The checker evaluates both while checking and records the result: the
// branch each `static if` takes, and each `static for` unrolled to its
// iterations. The core holds what lowering builds from those records: the
// taken branch's statements in place, and one block per iteration holding a
// `let` for each loop binding that has a literal form, then that
// iteration's copy of the body or yield. Reads of a binding with no literal
// form (a `field_info`'s `f.name`) are the literals the checker folded them
// to.

#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

namespace cinder::core {

auto body_builder::static_fold(const ast::expr &source) const
    -> const ast::expr * {
  if (static_folds_ == nullptr) {
    return nullptr;
  }
  const auto found = static_folds_->find(&source);
  if (found == static_folds_->end() || found->second == nullptr) {
    return nullptr;
  }
  // A local of the body's own with the binding's name shadows it, as in
  // lowering's `static_for_read_root` check.
  const auto *root = &source;
  while (root->kind == ast::node_kind::field_expr) {
    const auto *object = ast::cast<const ast::field_expr>(*root).object.get();
    if (object == nullptr) {
      return found->second;
    }
    root = object;
  }
  auto name = std::string{};
  if (root->kind == ast::node_kind::ident_expr) {
    name = ast::cast<const ast::ident_expr>(*root).name;
  } else if (root->kind == ast::node_kind::module_path_expr) {
    const auto &path = ast::cast<const ast::module_path_expr>(*root);
    if (!path.segments.empty()) {
      name = path.segments.front();
    }
  }
  for (const auto &scope : scopes_) {
    if (!name.empty() && scope.contains(name)) {
      return nullptr;
    }
  }
  return found->second;
}

auto body_builder::static_if_taken(const ast::static_decl &decl)
    -> result<bool> {
  for (const auto *frame : static_frames_ | std::views::reverse) {
    if (const auto found = frame->static_ifs.find(&decl);
        found != frame->static_ifs.end()) {
      return found->second;
    }
  }
  const auto taken = checked_.lowering.static_if_taken_branch.find(&decl);
  if (taken == checked_.lowering.static_if_taken_branch.end()) {
    return fail("missing record: static_if_taken_branch",
                "an unresolved `static if`", decl.span);
  }
  return taken->second;
}

auto body_builder::iteration_of(
    const semantic::static_for_iteration &iteration, source_span span,
    const std::function<result<ptr<expr>>()> &body) -> result<ptr<expr>> {
  push_scope();
  auto made = make_node<block>(span, unit_type());
  auto built = [&]() -> result<void> {
    for (const auto &[name, written] : iteration.bindings) {
      if (written == nullptr) {
        return fail("missing record: static_for_iterations",
                    std::format("the binding `{}` has no value", name), span);
      }
      CORE_TRY(value, value_of(*written));
      const auto type = value->type;
      made->statements.push_back(
          let_of(name, type, false, std::move(value), written->span));
    }
    const auto *saved_folds = std::exchange(static_folds_, &iteration.folded);
    static_frames_.push_back(&iteration);
    auto value = body();
    static_frames_.pop_back();
    static_folds_ = saved_folds;
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    made->type = (*value)->type;
    made->result = std::move(*value);
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::static_for_block_into(const ast::static_decl &decl,
                                         ptr_vec<stmt> &out) -> result<void> {
  // A loop inside another was unrolled with the enclosing iteration.
  const std::vector<semantic::static_for_iteration> *iterations = nullptr;
  if (!static_frames_.empty()) {
    for (const auto &[nested, unrolled] : static_frames_.back()->nested) {
      if (nested == &decl) {
        iterations = &unrolled;
      }
    }
  } else if (const auto found =
                 checked_.lowering.static_for_iterations.find(&decl);
             found != checked_.lowering.static_for_iterations.end()) {
    iterations = &found->second;
  }
  if (iterations == nullptr) {
    return fail("missing record: static_for_iterations",
                "a block `static for` that was not unrolled", decl.span);
  }
  for (const auto &iteration : *iterations) {
    CORE_TRY(copy, iteration_of(iteration, decl.span, [&]() -> result<ptr<expr>> {
      CORE_TRY(body, block_of(decl.for_body, decl.span, unit_type()));
      return ptr<expr>(std::move(body));
    }));
    // The body's value is unused: the iteration is a unit block holding it.
    auto &unrolled = static_cast<block &>(*copy);
    unrolled.statements.push_back(
        expr_stmt_of(std::move(unrolled.result), decl.span));
    unrolled.type = unit_type();
    out.push_back(expr_stmt_of(std::move(copy), decl.span));
  }
  return {};
}

auto body_builder::static_for_inline_of(const ast::static_decl &decl)
    -> result<ptr<expr>> {
  const auto elements = checked_.lowering.static_for_elements.find(&decl);
  const auto iterations = checked_.lowering.static_for_iterations.find(&decl);
  const auto dispatch = checked_.lowering.static_for_dispatches.find(&decl);
  if ((elements == checked_.lowering.static_for_elements.end() &&
       iterations == checked_.lowering.static_for_iterations.end()) ||
      dispatch == checked_.lowering.static_for_dispatches.end() ||
      decl.for_yield == nullptr) {
    return fail("missing record: static_for_iterations",
                "an inline `static for` that was not unrolled", decl.span);
  }
  const auto span = decl.span;
  CORE_TRY(list_type, settle(dispatch->second.list_type, span));
  push_scope();
  auto made = make_node<block>(span, list_type);
  auto built = [&]() -> result<void> {
    CORE_TRY(fresh, list_builder_call(dispatch->second.new_callee, {}, span));
    auto acc = let_of("<static for result>", fresh->type, true,
                      std::move(fresh), span);
    auto pushes = ptr_vec<stmt>{};
    if (elements != checked_.lowering.static_for_elements.end()) {
      for (const auto *element : elements->second) {
        if (element == nullptr) {
          return fail("missing record: static_for_elements",
                      "an element with no value", span);
        }
        CORE_TRY(value, value_of(*element));
        CORE_TRY(push, push_onto(*acc, dispatch->second, std::move(value),
                                 element->span));
        pushes.push_back(std::move(push));
      }
    } else {
      for (const auto &iteration : iterations->second) {
        CORE_TRY(value, iteration_of(iteration, span, [&]() {
                   return value_of(*decl.for_yield);
                 }));
        CORE_TRY(push,
                 push_onto(*acc, dispatch->second, std::move(value), span));
        pushes.push_back(std::move(push));
      }
    }
    auto result_place = place{};
    result_place.root.kind = place_root::origin::local;
    result_place.root.symbol = acc->symbol;
    result_place.root.name = acc->name;
    result_place.root.type = acc->type;
    made->statements.push_back(std::move(acc));
    for (auto &push : pushes) {
      made->statements.push_back(std::move(push));
    }
    CORE_TRY(result_value,
             coerce(use_of(std::move(result_place), span), list_type));
    made->result = std::move(result_value);
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::static_decl_into(const ast::static_decl &decl,
                                    ptr_vec<stmt> &out) -> result<void> {
  switch (decl.decl_kind) {
  case ast::static_decl_kind::assertion:
    return {};
  case ast::static_decl_kind::for_inline: {
    CORE_TRY(list, static_for_inline_of(decl));
    out.push_back(expr_stmt_of(std::move(list), decl.span));
    return {};
  }
  case ast::static_decl_kind::for_block:
    return static_for_block_into(decl, out);
  case ast::static_decl_kind::conditional_compilation: {
    CORE_TRY(taken, static_if_taken(decl));
    // The taken branch's statements splice into the enclosing list, as the
    // checker checked them, with no scope of their own.
    for (const auto &inner : taken ? decl.if_body : decl.else_body) {
      if (inner == nullptr) {
        continue;
      }
      if (is_pattern_let(*inner)) {
        return fail("let_stmt", "a destructuring `let` inside `static if`",
                    inner->span);
      }
      CORE_CHECK(stmt_into(*inner, out));
    }
    return {};
  }
  case ast::static_decl_kind::binding:
    break;
  }
  return fail("static_decl", "a local `static` binding", decl.span);
}

auto body_builder::static_tail_of(const ast::static_decl &decl, type_id type)
    -> std::optional<result<ptr<expr>>> {
  if (decl.decl_kind == ast::static_decl_kind::for_inline) {
    auto list = static_for_inline_of(decl);
    if (!list.has_value()) {
      return std::unexpected(list.error());
    }
    return coerce(std::move(*list), type);
  }
  if (decl.decl_kind == ast::static_decl_kind::conditional_compilation) {
    auto taken = static_if_taken(decl);
    if (!taken.has_value()) {
      return std::unexpected(taken.error());
    }
    auto branch =
        block_of(*taken ? decl.if_body : decl.else_body, decl.span, type);
    if (!branch.has_value()) {
      return std::unexpected(branch.error());
    }
    return ptr<expr>(std::move(*branch));
  }
  return std::nullopt;
}

} // namespace cinder::core
