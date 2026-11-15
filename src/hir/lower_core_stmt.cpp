#include <optional>
#include <string>
#include <utility>

#include "src/hir/lower_core_lowerer.h"

/// Statements and blocks.
namespace cinder::hir {

namespace {

/// The compound assignment `a op= b` the old lowerer keeps whole.
[[nodiscard]] auto compound_of(ast::binary_op op)
    -> std::optional<ast::assign_op> {
  using a = ast::assign_op;
  using b = ast::binary_op;
  switch (op) {
  case b::add: return a::add_assign;
  case b::sub: return a::sub_assign;
  case b::mul: return a::mul_assign;
  case b::div: return a::div_assign;
  case b::mod: return a::mod_assign;
  case b::bit_and: return a::and_assign;
  case b::bit_or: return a::or_assign;
  case b::bit_xor: return a::xor_assign;
  case b::shl: return a::shl_assign;
  case b::shr: return a::shr_assign;
  case b::add_wrap: return a::add_wrap_assign;
  case b::sub_wrap: return a::sub_wrap_assign;
  case b::mul_wrap: return a::mul_wrap_assign;
  case b::add_sat: return a::add_sat_assign;
  case b::sub_sat: return a::sub_sat_assign;
  case b::mul_sat: return a::mul_sat_assign;
  default: return std::nullopt;
  }
}

/// Whether evaluating `subscript` twice is the same as evaluating it once:
/// a literal or a read of a local.
[[nodiscard]] auto is_pure_subscript(const core::expr &subscript) -> bool {
  if (subscript.kind == core::node_kind::literal) {
    return true;
  }
  if (subscript.kind != core::node_kind::use) {
    return false;
  }
  const auto &read = static_cast<const core::use &>(subscript);
  return read.from.root.kind == core::place_root::origin::local &&
         read.from.path.empty();
}

[[nodiscard]] auto same_subscript(const core::expr &a, const core::expr &b)
    -> bool {
  if (a.kind != b.kind) {
    return false;
  }
  if (a.kind == core::node_kind::literal) {
    return static_cast<const core::literal &>(a).spelling ==
           static_cast<const core::literal &>(b).spelling;
  }
  return static_cast<const core::use &>(a).from.root.symbol ==
         static_cast<const core::use &>(b).from.root.symbol;
}

/// Whether `a` and `b` name the same place, each evaluated the same way: a
/// local root and steps whose subscripts are pure.
[[nodiscard]] auto same_place(const core::place &a, const core::place &b)
    -> bool {
  if (a.root.kind != core::place_root::origin::local ||
      b.root.kind != core::place_root::origin::local ||
      a.root.symbol != b.root.symbol || a.path.size() != b.path.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.path.size(); ++i) {
    const auto &x = a.path[i];
    const auto &y = b.path[i];
    if (x.kind != y.kind || x.index != y.index || x.name != y.name) {
      return false;
    }
    if (x.kind == core::projection::step::element &&
        (x.subscript == nullptr || y.subscript == nullptr ||
         !is_pure_subscript(*x.subscript) || !is_pure_subscript(*y.subscript) ||
         !same_subscript(*x.subscript, *y.subscript))) {
      return false;
    }
  }
  return true;
}

} // namespace

auto core_lowerer::declare(symbol_id symbol, const std::string &name)
    -> symbol_id {
  const auto made = mint();
  symbols_.insert_or_assign(symbol, made);
  names_.insert_or_assign(made, name);
  return made;
}

auto core_lowerer::block(const core::block &source, type_id type)
    -> core_result<ptr<hir_block>> {
  auto stmts = ptr_vec<hir_node>{};
  for (std::size_t i = 0; i < source.statements.size(); ++i) {
    const auto last = i + 1 == source.statements.size() && !source.result;
    auto done = stmt_into(*source.statements[i], stmts,
                          last ? std::optional{type} : std::nullopt);
    if (!done.has_value()) {
      return std::unexpected(done.error());
    }
  }
  if (source.result != nullptr) {
    auto value = expr(*source.result);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    const auto span = (*value)->span;
    stmts.push_back(hir::make<hir_expr_stmt>(span, std::move(*value)));
  }
  return hir::make<hir_block>(source.span, type, std::move(stmts));
}

auto core_lowerer::if_expr(const core::if_expr &source, type_id type)
    -> core_result<ptr<hir_if>> {
  auto branches = std::vector<hir_if_branch>{};
  auto else_body = ptr<hir_block>{};
  // An `elif` chain arrives as an `else` block holding the next `if`; the
  // old lowerer keeps the chain as the branches of one `hir_if`.
  for (const auto *current = &source; current != nullptr;) {
    auto condition = expr(*current->condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto body = block(*current->then_branch, type);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    branches.push_back(hir_if_branch{.condition = std::move(*condition),
                                     .body = std::move(*body)});
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
  return hir::make<hir_if>(source.span, type, std::move(branches),
                      std::move(else_body));
}

auto core_lowerer::stmt_into(const core::stmt &source, ptr_vec<hir_node> &out,
                             std::optional<type_id> tail_type)
    -> core_result<void> {
  const auto first = out.size();
  auto done = [&]() -> core_result<void> {
    using k = core::node_kind;
    switch (source.kind) {
    case k::let:
      return let_into(static_cast<const core::let &>(source), out);
    case k::assign:
      return assign_into(static_cast<const core::assign &>(source), out);
    case k::expr_stmt:
      return expr_stmt_into(static_cast<const core::expr_stmt &>(source), out,
                            tail_type);
    case k::while_loop:
      return while_into(static_cast<const core::while_loop &>(source), out);
    case k::return_stmt:
    case k::break_stmt:
    case k::continue_stmt:
      return jump_into(static_cast<const core::jump &>(source), out);
    default:
      return refuse(source);
    }
  }();
  if (!done.has_value()) {
    return done;
  }
  // The HIR statements made from one source statement end its temporaries
  // together (`make_references_explicit`).
  for (auto i = first; i < out.size(); ++i) {
    out[i]->continues_statement = i > first;
  }
  if (first < out.size() && out[first]->origin == nullptr) {
    out[first]->origin = source.origin;
  }
  return {};
}

auto core_lowerer::let_into(const core::let &source, ptr_vec<hir_node> &out)
    -> core_result<void> {
  if (source.value == nullptr) {
    return refuse("let with no value", source.span);
  }
  auto value = expr(*source.value);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  const auto symbol = declare(source.symbol, source.name);
  out.push_back(hir::make<hir_let>(source.span, symbol, source.name,
                              std::move(*value), source.is_mut));
  return {};
}

auto core_lowerer::assign_into(const core::assign &source,
                               ptr_vec<hir_node> &out) -> core_result<void> {
  // The core spells `a op= b` as `a = a op b`; HIR keeps the compound form,
  // which evaluates the place once.
  if (source.value->kind == core::node_kind::primitive) {
    const auto &combined = static_cast<const core::primitive &>(*source.value);
    if (combined.binary.has_value() && combined.operands.size() == 2 &&
        combined.operands[0]->kind == core::node_kind::use) {
      const auto op = compound_of(*combined.binary);
      const auto &read = static_cast<const core::use &>(*combined.operands[0]);
      if (op.has_value() && same_place(read.from, source.target)) {
        auto target = place(source.target, source.span);
        if (!target.has_value()) {
          return std::unexpected(target.error());
        }
        auto rhs = expr(*combined.operands[1]);
        if (!rhs.has_value()) {
          return std::unexpected(rhs.error());
        }
        out.push_back(hir::make<hir_assign>(source.span, *op, std::move(*target),
                                       std::move(*rhs)));
        return {};
      }
    }
  }
  auto target = place(source.target, source.span);
  if (!target.has_value()) {
    return std::unexpected(target.error());
  }
  auto value = expr(*source.value);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  out.push_back(hir::make<hir_assign>(source.span, ast::assign_op::assign,
                                 std::move(*target), std::move(*value)));
  return {};
}

auto core_lowerer::expr_stmt_into(const core::expr_stmt &source,
                                  ptr_vec<hir_node> &out,
                                  std::optional<type_id> tail_type)
    -> core_result<void> {
  const auto &value = *source.value;
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
  auto condition = ptr<hir_expr>{};
  if (source.condition != nullptr) {
    auto lowered = expr(*source.condition);
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
  if (source.step != nullptr) {
    auto lowered = block(*source.step, k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    step = std::move(*lowered);
  }
  out.push_back(hir::make<hir_while>(source.span, std::move(condition),
                                std::move(*body), std::move(step)));
  return {};
}

auto core_lowerer::jump_into(const core::jump &source, ptr_vec<hir_node> &out)
    -> core_result<void> {
  // Before a jump, the placeholder `make_references_explicit` fills with the
  // drops of the temporaries the jump leaves alive. A `return` of a value
  // drops them as part of its own full expression.
  const auto placeholder = [&](jump_exit exit) {
    auto made = hir::make<hir_block>(source.span, k_unknown_type,
                                ptr_vec<hir_node>{});
    made->exit = exit;
    out.push_back(hir::make<hir_expr_stmt>(source.span, std::move(made)));
  };
  switch (source.kind) {
  case core::node_kind::return_stmt: {
    if (source.value == nullptr) {
      placeholder(jump_exit::function);
      out.push_back(hir::make<hir_return>(source.span, nullptr));
      return {};
    }
    auto value = expr(*source.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    out.push_back(hir::make<hir_return>(source.span, std::move(*value)));
    return {};
  }
  case core::node_kind::break_stmt:
    placeholder(jump_exit::loop);
    out.push_back(hir::make<hir_break>(source.span));
    return {};
  case core::node_kind::continue_stmt:
    placeholder(jump_exit::loop);
    out.push_back(hir::make<hir_continue>(source.span));
    return {};
  default:
    return refuse(source);
  }
}

} // namespace cinder::hir
