#include <format>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "src/hir/captures.h"
#include "src/hir/lower_core_lowerer.h"

/// Lambdas: the closure, its environment and its drop glue.
namespace cinder::hir {

namespace {

[[nodiscard]] auto capture_mode_of(core::capture::mode how)
    -> ast::capture_mode {
  switch (how) {
  case core::capture::mode::copy: return ast::capture_mode::by_value;
  case core::capture::mode::move: return ast::capture_mode::by_move;
  case core::capture::mode::borrow: return ast::capture_mode::by_ref;
  case core::capture::mode::borrow_mut: return ast::capture_mode::by_mut_ref;
  }
  return ast::capture_mode::by_value;
}

[[nodiscard]] auto is_borrowed(core::capture::mode how) -> bool {
  return how == core::capture::mode::borrow ||
         how == core::capture::mode::borrow_mut;
}

/// A name with the copy suffix (`f#...`) of a generic lambda's copy
/// removed.
[[nodiscard]] auto base_name(const std::string &name) -> std::string {
  return name.substr(0, name.find('#'));
}

} // namespace

auto core_lowerer::lambda(const core::lambda &source)
    -> core_result<ptr<hir_expr>> {
  // Inside the body a capture is the outer local itself: HIR names the
  // enclosing binding, and the environment is built from those names.
  for (const auto &capture : source.captures) {
    const auto &root = capture.from.root;
    if (root.kind != core::place_root::origin::local ||
        !capture.from.path.empty()) {
      return refuse("capture of a place that is not a local", source.span);
    }
    if (aliases_.contains(root.symbol)) {
      return refuse("capture of a place alias", source.span);
    }
    const auto outer = symbols_.find(root.symbol);
    if (outer == symbols_.end()) {
      return refuse("capture of a local declared outside the supported forms",
                    source.span);
    }
    symbols_.insert_or_assign(capture.symbol, outer->second);
  }
  const auto return_type = hir_type(source.return_type);
  // The body is a function of its own: it neither suspends the enclosing
  // task nor waits ahead of the enclosing statement.
  const auto enclosing_task = std::exchange(in_task_body_, false);
  auto *const enclosing_hoist = std::exchange(hoist_, nullptr);
  const auto enclosing_conditional = std::exchange(conditional_depth_, 0);
  const auto enclosing_value_branch = std::exchange(value_branch_depth_, 0);
  auto enclosing_crews = std::exchange(open_crews_, {});
  push_scope();
  auto params = std::vector<hir_param>{};
  auto prelude = ptr_vec<hir_node>{};
  for (const auto &param : source.params) {
    const auto symbol = declare(param.symbol, param.name);
    params.push_back(hir_param{
        .symbol = symbol, .name = param.name, .type = hir_type(param.type)});
    if (param.origin != nullptr) {
      own(param.origin, symbol);
      drops_.declare_flags(param.origin, source.span, prelude);
    }
  }
  auto body = [&]() -> core_result<ptr<hir_block>> {
    // A destructuring parameter's bindings come first, in the parameters'
    // scope: each is a pattern `let` of the parameter holding the rest.
    const auto *rest = source.body.get();
    while (rest->origin == nullptr && rest->statements.empty() &&
           rest->result != nullptr &&
           rest->result->kind == core::node_kind::match_expr &&
           static_cast<const core::match_expr &>(*rest->result).form ==
               core::match_form::let_pattern) {
      auto next = let_pattern_into(
          static_cast<const core::match_expr &>(*rest->result), prelude);
      if (!next.has_value()) {
        return std::unexpected(next.error());
      }
      rest = *next;
    }
    const auto &written = *rest;
    // `=> value`: the body returns the value.
    if (written.origin == nullptr && written.statements.empty() &&
        written.result != nullptr) {
      auto value = expr(*written.result);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      auto stmts = std::move(prelude);
      stmts.push_back(
          hir::make<hir_return>(written.result->span, std::move(*value)));
      return hir::make<hir_block>(written.result->span, return_type,
                                  std::move(stmts));
    }
    auto made = block(written, return_type);
    if (made.has_value() && !prelude.empty()) {
      for (auto &stmt : (*made)->stmts) {
        prelude.push_back(std::move(stmt));
      }
      (*made)->stmts = std::move(prelude);
    }
    return made;
  }();
  pop_scope();
  in_task_body_ = enclosing_task;
  hoist_ = enclosing_hoist;
  conditional_depth_ = enclosing_conditional;
  value_branch_depth_ = enclosing_value_branch;
  open_crews_ = std::move(enclosing_crews);
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }
  auto closure = hir::make<hir_lambda>(source.span, source.type,
                                       std::move(params), return_type,
                                       std::move(*body));
  // The environment: the capture list as written; or, when the closure
  // borrows something, every local the body names with the borrowed ones
  // by reference; or else the set `capture_plan` computes from the body.
  if (source.capture_list) {
    auto list = std::vector<hir_capture>{};
    for (const auto &capture : source.captures) {
      list.push_back(hir_capture{.symbol = symbols_.at(capture.symbol),
                                 .mode = capture_mode_of(capture.how)});
    }
    closure->captures = std::move(list);
  } else if (std::ranges::any_of(source.captures, is_borrowed,
                                 &core::capture::how)) {
    auto modes = std::unordered_map<symbol_id, ast::capture_mode>{};
    for (const auto &capture : source.captures) {
      const auto outer = symbols_.at(capture.symbol);
      // One local captured twice (by value and by reference) is borrowed.
      const auto [entry, fresh] =
          modes.try_emplace(outer, capture_mode_of(capture.how));
      if (!fresh && is_borrowed(capture.how)) {
        entry->second = capture_mode_of(capture.how);
      }
    }
    auto plan = std::vector<hir_capture>{};
    for (const auto symbol : free_variables(*closure)) {
      const auto mode = modes.find(symbol);
      if (mode == modes.end()) {
        continue; // a function named by the body
      }
      plan.push_back(hir_capture{
          .symbol = symbol,
          .mode = ast::is_reference_capture(mode->second)
                      ? mode->second
                      : ast::capture_mode::by_value});
    }
    closure->captures = std::move(plan);
  }
  auto glue = drop_glue(source, *closure);
  if (!glue.has_value()) {
    return std::unexpected(glue.error());
  }
  closure->drop_glue = std::move(*glue);
  return ptr<hir_expr>(std::move(closure));
}

auto core_lowerer::drop_glue(const core::lambda &source,
                             const hir_lambda &closure)
    -> core_result<ptr<hir_lambda>> {
  const auto owned =
      drops_.schedule().owned_captures.find(source.closure_origin);
  if (source.closure_origin == nullptr ||
      owned == drops_.schedule().owned_captures.end()) {
    return ptr<hir_lambda>{};
  }
  const auto plan = checked_.drop.drop_plans.find(closure.type);
  if (plan == checked_.drop.drop_plans.end() ||
      !plan->second.closure_glue.has_value()) {
    return refuse("closure owning captures with no drop glue type",
                  source.span);
  }
  const auto glue_type = *plan->second.closure_glue;
  const auto unit = checked_.types.entry(glue_type).result;
  // Last captured, first dropped, as an ordinary scope drops.
  auto stmts = ptr_vec<hir_node>{};
  for (const auto &capture : std::views::reverse(owned->second)) {
    // The capture the record names: the local `name`, which for a generic
    // lambda's name is every copy made for it.
    auto parts = std::vector<std::pair<symbol_id, type_id>>{};
    for (const auto &held : source.captures) {
      if (held.from.root.name == capture.name) {
        parts.emplace_back(symbols_.at(held.symbol), capture.type);
        break;
      }
    }
    if (parts.empty()) {
      for (const auto &held : source.captures) {
        const auto type = hir_type(held.type);
        if (base_name(held.from.root.name) == capture.name &&
            checked_.drop.drop_plans.contains(type)) {
          parts.emplace_back(symbols_.at(held.symbol), type);
        }
      }
    }
    if (parts.empty()) {
      return refuse("owned capture the closure does not capture",
                    source.span);
    }
    for (const auto &[part, type] : parts) {
      const auto span = source.span;
      const auto name = capture.name;
      const place_fn place = [span, part, type, name]() -> ptr<hir_expr> {
        return ptr<hir_expr>(hir::make<hir_local_ref>(span, type, part, name));
      };
      if (auto drops = drops_.build_drop_calls(place, type, span, stmts);
          !drops.has_value()) {
        return refuse_drop(drops.error());
      }
    }
  }
  // The closure's own environment, so the glue reads the same block.
  return hir::make<hir_lambda>(
      source.span, glue_type, std::vector<hir_param>{}, unit,
      hir::make<hir_block>(source.span, unit, std::move(stmts)),
      capture_plan(closure));
}

} // namespace cinder::hir
