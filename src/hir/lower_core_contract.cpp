#include <algorithm>
#include <format>
#include <optional>
#include <utility>
#include <vector>

#include "src/hir/lower_core_lowerer.h"

/// Contracts: `pre`, `post` and `invariant` checks, and the exits that check
/// postconditions or end crews on the way out.
namespace cinder::hir {

namespace {

[[nodiscard]] auto contract_kind_of(core::contract_check::clause kind)
    -> contract_kind {
  switch (kind) {
  case core::contract_check::clause::pre: return contract_kind::precondition;
  case core::contract_check::clause::post: return contract_kind::postcondition;
  case core::contract_check::clause::invariant: return contract_kind::invariant;
  }
  return contract_kind::precondition;
}

[[nodiscard]] auto is_post_check(const core::stmt &statement) -> bool {
  return statement.kind == core::node_kind::contract_check &&
         static_cast<const core::contract_check &>(statement).kind ==
             core::contract_check::clause::post;
}

[[nodiscard]] auto is_crew_end(const core::stmt &statement) -> bool {
  if (statement.kind != core::node_kind::expr_stmt) {
    return false;
  }
  const auto *value = static_cast<const core::expr_stmt &>(statement).value.get();
  return value != nullptr && value->kind == core::node_kind::call &&
         static_cast<const core::call *>(value)->target.kind ==
             core::callee::form::intrinsic &&
         static_cast<const core::call *>(value)->target.name == "crew_end";
}

} // namespace

auto core_lowerer::exit_block(const core::node *node) -> const core::block * {
  if (node != nullptr && node->kind == core::node_kind::expr_stmt) {
    node = static_cast<const core::expr_stmt *>(node)->value.get();
  }
  if (node == nullptr || node->kind != core::node_kind::block) {
    return nullptr;
  }
  const auto &exit = static_cast<const core::block &>(*node);
  if (exit.origin != nullptr || exit.result != nullptr ||
      exit.statements.size() < 2 ||
      exit.statements.back()->kind != core::node_kind::return_stmt) {
    return nullptr;
  }
  for (std::size_t i = 0; i + 1 < exit.statements.size(); ++i) {
    const auto &statement = *exit.statements[i];
    const auto bound = i == 0 && statement.kind == core::node_kind::let &&
                       static_cast<const core::let &>(statement).name ==
                           "return";
    if (!bound && !is_post_check(statement) && !is_crew_end(statement)) {
      return nullptr;
    }
  }
  return &exit;
}

auto core_lowerer::contract_into(const core::contract_check &source,
                                 ptr_vec<hir_node> &out) -> core_result<void> {
  // Without contract checks the old lowerer leaves out every check and the
  // binding each one names its value by; lowered here, the bindings would
  // stay.
  if (!options_.contract_checks) {
    return refuse("contract_check without contract checks", source.span);
  }
  auto condition = expr(*source.condition);
  if (!condition.has_value()) {
    return std::unexpected(condition.error());
  }
  out.push_back(hir::make<hir_contract_check>(
      source.span, std::move(*condition), contract_kind_of(source.kind),
      source.message));
  return {};
}

auto core_lowerer::exit_into(const core::block &exit, ptr_vec<hir_node> &out,
                             ptr<hir_expr> tail) -> core_result<void> {
  const auto &leave = static_cast<const core::jump &>(*exit.statements.back());
  const auto &first = *exit.statements.front();
  const auto *bound = first.kind == core::node_kind::let
                          ? static_cast<const core::let *>(&first)
                          : nullptr;
  auto value = std::move(tail);
  if (value == nullptr && bound != nullptr) {
    auto lowered = expr(*bound->value);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    value = std::move(*lowered);
  }
  // The crews it leaves end, then the temporaries and the scheduled drops,
  // as before any `return`.
  const auto exit_drops = [&]() -> core_result<void> {
    for (const auto &statement : exit.statements) {
      if (is_crew_end(*statement)) {
        if (auto ended = stmt_into(*statement, out, std::nullopt); !ended) {
          return ended;
        }
      }
    }
    if (leave.origin == nullptr) {
      return {};
    }
    auto dropped =
        drops_.emit_jump_drops(leave.origin, jump_exit::function, leave.span,
                               out);
    if (!dropped.has_value()) {
      return refuse_drop(dropped.error());
    }
    return {};
  };
  if (value == nullptr) {
    if (auto dropped = exit_drops(); !dropped) {
      return dropped;
    }
    out.push_back(hir::make<hir_return>(leave.span, nullptr));
    return {};
  }
  const auto scheduled = leave.origin != nullptr
                             ? drops_.schedule().exits.find(leave.origin)
                             : drops_.schedule().exits.end();
  const auto has_drops = scheduled != drops_.schedule().exits.end() &&
                         !scheduled->second.empty();
  const auto has_crews = std::ranges::any_of(
      exit.statements, [](const auto &statement) -> bool {
        return is_crew_end(*statement);
      });
  if ((has_drops || has_crews) && value->type != k_unknown_type) {
    const auto value_type = value->type;
    const auto temp = mint();
    const auto temp_name = std::format("<return value {}>", temp);
    out.push_back(
        hir::make<hir_let>(leave.span, temp, temp_name, std::move(value)));
    if (auto dropped = exit_drops(); !dropped) {
      return dropped;
    }
    value = hir::make<hir_local_ref>(source_span::dummy(), value_type, temp,
                                     temp_name);
  } else if (leave.origin != nullptr) {
    if (auto dropped = exit_drops(); !dropped) {
      return dropped;
    }
  }
  auto posts = std::vector<const core::contract_check *>{};
  for (const auto &statement : exit.statements) {
    if (is_post_check(*statement)) {
      posts.push_back(static_cast<const core::contract_check *>(statement.get()));
    }
  }
  return return_through_posts(
      leave.span, std::move(value),
      bound != nullptr ? std::optional{bound->symbol} : std::nullopt, posts,
      out);
}

auto core_lowerer::return_through_posts(
    source_span span, ptr<hir_expr> value,
    std::optional<symbol_id> bound,
    const std::vector<const core::contract_check *> &posts,
    ptr_vec<hir_node> &out) -> core_result<void> {
  // A task hands its result over with a `yield`, unchecked, as before.
  if (in_task_body_) {
    out.push_back(hir::make<hir_yield>(span, std::move(value)));
    out.push_back(hir::make<hir_return>(span, nullptr));
    return {};
  }
  if (posts.empty()) {
    out.push_back(hir::make<hir_return>(span, std::move(value)));
    return {};
  }
  // `let return = value; <post checks>; return return`.
  const auto type = value->type;
  const auto symbol = bound.has_value() ? declare(*bound, "return") : mint();
  out.push_back(hir::make<hir_let>(span, symbol, std::string("return"),
                                   std::move(value)));
  for (const auto *post : posts) {
    if (auto checked = contract_into(*post, out); !checked) {
      return checked;
    }
  }
  out.push_back(hir::make<hir_return>(
      span, hir::make<hir_local_ref>(span, type, symbol, std::string("return"))));
  return {};
}

auto core_lowerer::finish_contracts(ptr_vec<hir_node> &stmts, source_span span)
    -> core_result<void> {
  const auto *exit = std::exchange(tail_exit_, nullptr);
  auto posts = std::exchange(tail_posts_, {});
  if (exit == nullptr && posts.empty()) {
    return {};
  }
  auto *tail = stmts.empty() ? nullptr : stmts.back().get();
  if (tail != nullptr && always_returns(*tail)) {
    return {};
  }
  if (tail != nullptr && tail->kind == hir_node_kind::hir_expr_stmt) {
    auto value = std::move(dynamic_cast<hir_expr_stmt &>(*tail).expr);
    const auto tail_span = tail->span;
    stmts.pop_back();
    if (exit != nullptr) {
      return exit_into(*exit, stmts, std::move(value));
    }
    return return_through_posts(tail_span, std::move(value), std::nullopt,
                                posts, stmts);
  }
  if (exit != nullptr) {
    return refuse("postcondition exit with no tail value", span);
  }
  for (const auto *post : posts) {
    if (auto checked = contract_into(*post, stmts); !checked) {
      return checked;
    }
  }
  return {};
}

auto core_lowerer::tail_contracts(const core::block &source)
    -> std::size_t {
  auto count = source.statements.size();
  if (&source != function_body_ || source.result != nullptr || count == 0) {
    return count;
  }
  if (const auto *exit = exit_block(source.statements.back().get());
      exit != nullptr &&
      static_cast<const core::jump &>(*exit->statements.back()).origin ==
          nullptr &&
      exit->statements.front()->kind == core::node_kind::let) {
    tail_exit_ = exit;
    return count - 1;
  }
  while (count > 0 && is_post_check(*source.statements[count - 1])) {
    --count;
  }
  for (auto i = count; i < source.statements.size(); ++i) {
    tail_posts_.push_back(
        static_cast<const core::contract_check *>(source.statements[i].get()));
  }
  return count;
}

auto core_lowerer::invariant_block(const core::expr &value)
    -> const core::block * {
  if (value.kind != core::node_kind::block) {
    return nullptr;
  }
  const auto &checked = static_cast<const core::block &>(value);
  if (checked.origin != nullptr || checked.result != nullptr ||
      checked.statements.empty() ||
      checked.statements.back()->kind != core::node_kind::contract_check ||
      static_cast<const core::contract_check &>(*checked.statements.back())
              .kind != core::contract_check::clause::invariant) {
    return nullptr;
  }
  return &checked;
}

} // namespace cinder::hir
