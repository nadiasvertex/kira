// Contracts and type invariants checked at run time (spec/unified-checker-
// plan.md, phase 2).
//
// A `pre` or `post` the checker could not prove (not in `elided_contracts`)
// and a type's `invariant` become `contract_check` statements where lowering
// places them: preconditions first in the body; postconditions at each
// return, with the returned value bound to a local named `return`; an
// invariant after a struct literal of the type, with the value bound to
// `self`, and after a write to one of its fields, with `self` a shared
// borrow of the object written.

#include <format>
#include <string>
#include <utility>

#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"

namespace cinder::core {

auto body_builder::contract_check_of(const ast::expr &condition,
                                     std::string message, source_span span,
                                     contract_check::clause kind)
    -> result<ptr<stmt>> {
  CORE_TRY(value, value_as(condition, checked_.types.bool_type()));
  auto made = make_node<contract_check>(span);
  made->condition = std::move(value);
  made->message = std::move(message);
  made->kind = kind;
  return ptr<stmt>(std::move(made));
}

auto body_builder::post_checks_into(ptr_vec<stmt> &out) -> result<void> {
  for (const auto *contract : post_contracts_) {
    CORE_TRY(check, contract_check_of(*contract->condition,
                                      contract->message.value_or(std::string{}),
                                      contract->span,
                                      contract_check::clause::post));
    out.push_back(std::move(check));
  }
  return {};
}

auto body_builder::return_of(ptr<expr> value, source_span span,
                             const void *origin) -> result<ptr<stmt>> {
  if (post_contracts_.empty() && open_crews_.empty()) {
    auto made = make_node<jump>(node_kind::return_stmt, span);
    made->origin = origin;
    made->value = std::move(value);
    return ptr<stmt>(std::move(made));
  }
  // `let return = value; <post checks>; <crew ends>; return return`.
  push_scope();
  auto exit = make_node<block>(span, unit_type());
  auto built = [&]() -> result<void> {
    auto leave = make_node<jump>(node_kind::return_stmt, span);
    leave->origin = origin;
    if (value != nullptr) {
      const auto type = value->type;
      auto bound = let_of("return", type, false, std::move(value), span);
      auto read = place{};
      read.root.kind = place_root::origin::local;
      read.root.symbol = bound->symbol;
      read.root.name = bound->name;
      read.root.type = type;
      exit->statements.push_back(std::move(bound));
      leave->value = use_of(std::move(read), span);
    }
    CORE_CHECK(post_checks_into(exit->statements));
    crew_exits_into(false, span, exit->statements);
    exit->statements.push_back(std::move(leave));
    return {};
  }();
  pop_scope();
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  return expr_stmt_of(std::move(exit), span);
}

auto body_builder::contracts_into(const ast::func_decl &decl, block &body)
    -> result<void> {
  auto pre = ptr_vec<stmt>{};
  for (const auto &contract : decl.contracts) {
    if (contract.condition == nullptr ||
        checked_.lowering.elided_contracts.contains(&contract)) {
      continue;
    }
    if (contract.condition->has_error) {
      return fail("error node", "a contract carries a parse error",
                  contract.span);
    }
    if (!contract.is_pre) {
      continue;
    }
    CORE_TRY(check,
             contract_check_of(*contract.condition,
                               contract.message.value_or(std::string{}),
                               contract.span, contract_check::clause::pre));
    pre.push_back(std::move(check));
  }
  if (!pre.empty()) {
    for (auto &statement : body.statements) {
      pre.push_back(std::move(statement));
    }
    body.statements = std::move(pre);
  }
  if (post_contracts_.empty()) {
    return {};
  }
  // A valued tail returns through the checks; a body that runs off its end
  // checks last; one that ends in a `return` has checked already.
  if (body.result != nullptr) {
    const auto span = body.result->span;
    CORE_TRY(exit, return_of(std::move(body.result), span));
    body.statements.push_back(std::move(exit));
    return {};
  }
  if (!body.statements.empty() &&
      body.statements.back()->kind == node_kind::return_stmt) {
    return {};
  }
  return post_checks_into(body.statements);
}

auto body_builder::invariant_of(type_id type) const -> const ast::expr * {
  const auto &entry = checked_.types.entry(type);
  if (entry.decl == nullptr || entry.decl->invariant == nullptr ||
      entry.decl->invariant->has_error) {
    return nullptr;
  }
  return entry.decl->invariant.get();
}

auto body_builder::invariant_check_of(const ast::expr &invariant,
                                      ptr<expr> self_value, source_span span,
                                      ptr_vec<stmt> &out)
    -> result<symbol_id> {
  const auto type = self_value->type;
  auto bound = let_of("self", type, false, std::move(self_value), span);
  const auto symbol = bound->symbol;
  out.push_back(std::move(bound));
  CORE_TRY(check, contract_check_of(invariant, std::string{}, span,
                                    contract_check::clause::invariant));
  out.push_back(std::move(check));
  return symbol;
}

auto body_builder::checked_construction(ptr<expr> value)
    -> result<ptr<expr>> {
  const auto *invariant = invariant_of(value->type);
  if (invariant == nullptr) {
    return value;
  }
  const auto type = value->type;
  const auto span = value->span;
  push_scope();
  auto made = make_node<block>(span, type);
  auto self = invariant_check_of(*invariant, std::move(value), span,
                                 made->statements);
  pop_scope();
  if (!self.has_value()) {
    return std::unexpected(self.error());
  }
  auto read = place{};
  read.root.kind = place_root::origin::local;
  read.root.symbol = *self;
  read.root.name = "self";
  read.root.type = type;
  made->result = use_of(std::move(read), span);
  return ptr<expr>(std::move(made));
}

auto body_builder::clone_place(const place &p, source_span span)
    -> result<place> {
  if (p.root.kind == place_root::origin::temporary) {
    return fail("type invariant",
                "a field write to a type with an invariant through a "
                "temporary, which cannot be read again",
                span);
  }
  auto copy = place{};
  copy.root.kind = p.root.kind;
  copy.root.symbol = p.root.symbol;
  copy.root.name = p.root.name;
  copy.root.module = p.root.module;
  copy.root.type = p.root.type;
  for (const auto &step : p.path) {
    if (step.kind == projection::step::element) {
      return fail("type invariant",
                  "a field write to a type with an invariant through an "
                  "element, which cannot be read again",
                  span);
    }
    auto next = projection{};
    next.kind = step.kind;
    next.type = step.type;
    next.name = step.name;
    next.index = step.index;
    copy.path.push_back(std::move(next));
  }
  return copy;
}

auto body_builder::field_write_checks_into(const place &target,
                                           source_span span,
                                           ptr_vec<stmt> &out)
    -> result<void> {
  if (target.path.empty() ||
      target.path.back().kind != projection::step::field) {
    return {};
  }
  const auto object_type = target.path.size() >= 2
                               ? target.path[target.path.size() - 2].type
                               : target.root.type;
  const auto *invariant = invariant_of(object_type);
  if (invariant == nullptr) {
    return {};
  }
  CORE_TRY(object, clone_place(target, span));
  object.path.pop_back();
  // `self` is a shared borrow of the object written, as lowering binds it.
  auto lent = make_node<borrow>(span, checked_.types.ref_to(object.type(), false));
  lent->of = std::move(object);
  push_scope();
  auto checked = make_node<block>(span, unit_type());
  auto self = invariant_check_of(*invariant, std::move(lent), span,
                                 checked->statements);
  pop_scope();
  if (!self.has_value()) {
    return std::unexpected(self.error());
  }
  out.push_back(expr_stmt_of(std::move(checked), span));
  return {};
}

} // namespace cinder::core
