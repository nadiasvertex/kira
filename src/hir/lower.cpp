#include "src/hir/lower.h"

#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/hir/ids.h"
#include "src/hir/nodes.h"
#include "src/k-parser/ast.h"
#include "src/semantic/types.h"

namespace kira::hir {

namespace {

using semantic::checked_types;
using semantic::k_error_type;
using semantic::k_unknown_type;
using semantic::type_id;

[[nodiscard]] auto fail(lowering_error_kind kind, source_span span,
                        std::string message)
    -> std::unexpected<lowering_error> {
  return std::unexpected(lowering_error{
      .kind = kind, .span = span, .message = std::move(message)});
}

/// Wraps a freshly-built derived node into the `ptr<hir_expr>` result type
/// every expression-lowering helper returns.
template <typename T>
[[nodiscard]] auto ok_expr(ptr<T> node)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  return ptr<hir_expr>(std::move(node));
}

/// Performs the AST-to-HIR walk for one function at a time. Not reusable
/// across functions: `scopes_`/`global_refs_`/`next_symbol_` are lowering-
/// local bookkeeping, reset per `lower_function` call (see the class-level
/// comment on `symbol_id` below).
class lowerer {
public:
  explicit lowerer(const checked_types &checked) : checked_(checked) {}

  [[nodiscard]] auto lower_function(const ast::func_decl &decl)
      -> std::expected<ptr<hir_function>, lowering_error>;

private:
  // ------------------------------------------------------------------
  //  Local identity
  //
  //  Decision 7 (spec/typed-ir-design.md) says HIR should reuse the
  //  semantic session's `symbol_id` rather than reinvent identity — but
  //  `checked_types` (src/semantic/types.h) only persists *types*, not the
  //  session's symbol table, so that real id isn't available here yet.
  //  Until that's threaded through, this class mints its own ordinals: a
  //  fresh id per parameter/`let` binding as it's declared, and one fixed
  //  id per distinct free-standing name (e.g. a called function) the first
  //  time it's referenced. These ids are only unique *within one
  //  `lower_function` call* — they are not stable across functions and
  //  must not be persisted or compared across separate lowering calls.
  // ------------------------------------------------------------------

  auto push_scope() -> void { scopes_.emplace_back(); }
  auto pop_scope() -> void { scopes_.pop_back(); }

  [[nodiscard]] auto declare_local(std::string_view name) -> symbol_id {
    const auto id = next_symbol_++;
    scopes_.back().emplace(std::string(name), id);
    return id;
  }

  /// Mints an id with no name of its own — used for a `match` subject's
  /// synthetic binding, which arm code never refers to by spelling (see
  /// `lower_match`).
  [[nodiscard]] auto mint_symbol() -> symbol_id { return next_symbol_++; }

  [[nodiscard]] auto resolve_reference(std::string_view name) -> symbol_id {
    for (auto & scope : std::views::reverse(scopes_)) {
      if (const auto found = scope.find(std::string(name)); found != scope.end()) {
        return found->second;
      }
    }
    auto key = std::string(name);
    if (const auto found = global_refs_.find(key);
        found != global_refs_.end()) {
      return found->second;
    }
    const auto id = next_symbol_++;
    global_refs_.emplace(std::move(key), id);
    return id;
  }

  // ------------------------------------------------------------------
  //  Checked-type lookup
  // ------------------------------------------------------------------

  [[nodiscard]] auto checked_type_of(const ast::node &node)
      -> std::expected<type_id, lowering_error> {
    const auto found = checked_.node_types.find(&node);
    if (found == checked_.node_types.end() || found->second == k_unknown_type ||
        found->second == k_error_type) {
      return fail(lowering_error_kind::unresolved_type, node.span,
                  "no concrete checked type is available for this node; "
                  "lowering only accepts fully type-checked, fully-annotated "
                  "code (spec/typed-ir-design.md Decision 1)");
    }
    return found->second;
  }

  // ------------------------------------------------------------------
  //  Expressions
  // ------------------------------------------------------------------

  [[nodiscard]] auto lower_expr(const ast::expr &expr)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_literal(const ast::literal_expr &lit)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_ident(const ast::ident_expr &ident)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_binary(const ast::binary_expr &bin)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_unary(const ast::unary_expr &un)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_call(const ast::call_expr &call)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_field(const ast::field_expr &field)
      -> std::expected<ptr<hir_expr>, lowering_error>;
  [[nodiscard]] auto lower_index(const ast::index_expr &index)
      -> std::expected<ptr<hir_expr>, lowering_error>;

  // ------------------------------------------------------------------
  //  Statements, blocks, and if (shared between statement and expression
  //  position — see hir::hir_if's doc comment).
  // ------------------------------------------------------------------

  [[nodiscard]] auto lower_block(const std::vector<ast::ptr<ast::node>> &stmts,
                                 source_span span,
                                 type_id type = k_unknown_type)
      -> std::expected<ptr<hir_block>, lowering_error>;
  [[nodiscard]] auto lower_stmt(const ast::node &node)
      -> std::expected<ptr<hir_node>, lowering_error>;
  [[nodiscard]] auto lower_if(const std::vector<ast::if_branch> &branches,
                              const std::vector<ast::ptr<ast::node>> &else_body,
                              source_span span, type_id type)
      -> std::expected<ptr<hir_if>, lowering_error>;

  // ------------------------------------------------------------------
  //  match / patterns
  // ------------------------------------------------------------------

  /// Lowers one surface pattern to a *structural* `hir_pattern` (wildcard,
  /// literal, or a `|` of those). A plain binding or a `pattern as name`
  /// alias contributes no structure of its own — instead it appends a
  /// synthetic `hir_let name = <subject_symbol>` to `pending`, which the
  /// caller (`lower_match`) splices onto the front of the arm's body.
  [[nodiscard]] auto
  lower_pattern(const ast::node &pattern, symbol_id subject_symbol,
                type_id subject_type, std::vector<ptr<hir_node>> &pending)
      -> std::expected<ptr<hir_pattern>, lowering_error>;
  [[nodiscard]] auto lower_match(const ast::expr &subject_ast,
                                 const std::vector<ast::match_arm> &arms,
                                 source_span span, type_id type)
      -> std::expected<ptr<hir_match>, lowering_error>;

  const checked_types &checked_;
  std::vector<std::unordered_map<std::string, symbol_id>> scopes_;
  std::unordered_map<std::string, symbol_id> global_refs_;
  symbol_id next_symbol_ = 0;
};

auto lowerer::lower_expr(const ast::expr &expr)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  if (expr.has_error) {
    return fail(
        lowering_error_kind::unsupported_construct, expr.span,
        "expression carries a parse/recovery error and cannot be lowered");
  }
  switch (expr.kind) {
  case ast::node_kind::group_expr: {
    const auto &group = dynamic_cast<const ast::group_expr &>(expr);
    if (group.inner == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, expr.span,
                  "parenthesized expression has no inner expression");
    }
    return lower_expr(*group.inner);
  }
  case ast::node_kind::literal_expr:
    return lower_literal(dynamic_cast<const ast::literal_expr &>(expr));
  case ast::node_kind::ident_expr:
    return lower_ident(dynamic_cast<const ast::ident_expr &>(expr));
  case ast::node_kind::binary_expr:
    return lower_binary(dynamic_cast<const ast::binary_expr &>(expr));
  case ast::node_kind::unary_expr:
    return lower_unary(dynamic_cast<const ast::unary_expr &>(expr));
  case ast::node_kind::call_expr:
    return lower_call(dynamic_cast<const ast::call_expr &>(expr));
  case ast::node_kind::field_expr:
    return lower_field(dynamic_cast<const ast::field_expr &>(expr));
  case ast::node_kind::index_expr:
    return lower_index(dynamic_cast<const ast::index_expr &>(expr));
  case ast::node_kind::block_expr: {
    const auto &block = dynamic_cast<const ast::block_expr &>(expr);
    auto type = checked_type_of(block);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered = lower_block(block.stmts, block.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  case ast::node_kind::if_expr: {
    const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
    auto type = checked_type_of(if_e);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered = lower_if(if_e.branches, if_e.else_body, if_e.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  case ast::node_kind::match_expr: {
    const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
    if (match_e.subject == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, match_e.span,
                  "match expression has no subject");
    }
    auto type = checked_type_of(match_e);
    if (!type.has_value()) {
      return std::unexpected(type.error());
    }
    auto lowered =
        lower_match(*match_e.subject, match_e.arms, match_e.span, *type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ok_expr(std::move(*lowered));
  }
  default:
    return fail(lowering_error_kind::unsupported_construct, expr.span,
                std::format("expression kind {} is not lowered by the first "
                            "milestone (spec/typed-ir-design.md)",
                            static_cast<int>(expr.kind)));
  }
}

auto lowerer::lower_literal(const ast::literal_expr &lit)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(lit);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  return ok_expr(make<hir_literal>(lit.span, *type, lit.lit_kind, lit.value));
}

auto lowerer::lower_ident(const ast::ident_expr &ident)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(ident);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  const auto symbol = resolve_reference(ident.name);
  return ok_expr(make<hir_local_ref>(ident.span, *type, symbol, ident.name));
}

auto lowerer::lower_binary(const ast::binary_expr &bin)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(bin);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (bin.lhs == nullptr || bin.rhs == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, bin.span,
                "binary expression is missing an operand");
  }
  auto lhs = lower_expr(*bin.lhs);
  if (!lhs.has_value()) {
    return std::unexpected(lhs.error());
  }
  auto rhs = lower_expr(*bin.rhs);
  if (!rhs.has_value()) {
    return std::unexpected(rhs.error());
  }
  return ok_expr(hir::make<hir_binary>(bin.span, *type, bin.op, std::move(*lhs),
                                       std::move(*rhs)));
}

auto lowerer::lower_unary(const ast::unary_expr &un)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(un);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (un.operand == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, un.span,
                "unary expression is missing its operand");
  }
  auto operand = lower_expr(*un.operand);
  if (!operand.has_value()) {
    return std::unexpected(operand.error());
  }
  return ok_expr(
      hir::make<hir_unary>(un.span, *type, un.op, std::move(*operand)));
}

auto lowerer::lower_call(const ast::call_expr &call)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(call);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  for (const auto &arg : call.args) {
    if (arg.name.has_value()) {
      return fail(lowering_error_kind::unsupported_construct, arg.span,
                  "named call arguments are not lowered by the first "
                  "milestone (positional arguments only)");
    }
  }
  if (call.callee == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, call.span,
                "call expression is missing its callee");
  }
  auto callee = lower_expr(*call.callee);
  if (!callee.has_value()) {
    return std::unexpected(callee.error());
  }
  auto args = ptr_vec<hir_expr>{};
  args.reserve(call.args.size());
  for (const auto &arg : call.args) {
    if (arg.value == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, arg.span,
                  "call argument is missing its value");
    }
    auto lowered = lower_expr(*arg.value);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    args.push_back(std::move(*lowered));
  }
  return ok_expr(
      make<hir_call>(call.span, *type, std::move(*callee), std::move(args)));
}

auto lowerer::lower_field(const ast::field_expr &field)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(field);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (!field.generic_args.empty()) {
    return fail(lowering_error_kind::unsupported_construct, field.span,
                "generic method-call arguments are not lowered by the first "
                "milestone");
  }
  if (field.object == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, field.span,
                "field access is missing its target expression");
  }
  auto object = lower_expr(*field.object);
  if (!object.has_value()) {
    return std::unexpected(object.error());
  }
  return ok_expr(
      make<hir_field>(field.span, *type, std::move(*object), field.field_name));
}

auto lowerer::lower_index(const ast::index_expr &index)
    -> std::expected<ptr<hir_expr>, lowering_error> {
  auto type = checked_type_of(index);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  if (index.object == nullptr || index.index == nullptr) {
    return fail(lowering_error_kind::unsupported_construct, index.span,
                "index expression is missing its target or index");
  }
  auto object = lower_expr(*index.object);
  if (!object.has_value()) {
    return std::unexpected(object.error());
  }
  auto idx = lower_expr(*index.index);
  if (!idx.has_value()) {
    return std::unexpected(idx.error());
  }
  return ok_expr(
      make<hir_index>(index.span, *type, std::move(*object), std::move(*idx)));
}

auto lowerer::lower_block(const std::vector<ast::ptr<ast::node>> &stmts,
                          source_span span, type_id type)
    -> std::expected<ptr<hir_block>, lowering_error> {
  push_scope();
  auto lowered_stmts = ptr_vec<hir_node>{};
  lowered_stmts.reserve(stmts.size());
  for (const auto &stmt_ptr : stmts) {
    if (stmt_ptr == nullptr) {
      continue;
    }
    auto lowered = lower_stmt(*stmt_ptr);
    if (!lowered.has_value()) {
      pop_scope();
      return std::unexpected(lowered.error());
    }
    lowered_stmts.push_back(std::move(*lowered));
  }
  pop_scope();
  return make<hir_block>(span, type, std::move(lowered_stmts));
}

auto lowerer::lower_stmt(const ast::node &node)
    -> std::expected<ptr<hir_node>, lowering_error> {
  if (node.has_error) {
    return fail(
        lowering_error_kind::unsupported_construct, node.span,
        "statement carries a parse/recovery error and cannot be lowered");
  }
  switch (node.kind) {
  case ast::node_kind::let_stmt: {
    const auto &let = dynamic_cast<const ast::let_stmt &>(node);
    if (!let.else_body.empty()) {
      return fail(lowering_error_kind::unsupported_construct, let.span,
                  "`let ... else` is not lowered by the first milestone");
    }
    if (let.pattern == nullptr || let.pattern->has_error ||
        let.pattern->kind != ast::node_kind::binding_pattern) {
      return fail(lowering_error_kind::unsupported_construct, let.span,
                  "only simple `let name = expr` bindings are lowered by the "
                  "first milestone (no destructuring patterns yet)");
    }
    if (let.initializer == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, let.span,
                  "let binding has no initializer");
    }
    auto initializer = lower_expr(*let.initializer);
    if (!initializer.has_value()) {
      return std::unexpected(initializer.error());
    }
    const auto &binding =
        dynamic_cast<const ast::binding_pattern &>(*let.pattern);
    const auto symbol = declare_local(binding.name);
    return ptr<hir_node>(
        make<hir_let>(let.span, symbol, binding.name, std::move(*initializer)));
  }
  case ast::node_kind::expr_stmt: {
    const auto &expr_stmt = dynamic_cast<const ast::expr_stmt &>(node);
    if (expr_stmt.expr == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, expr_stmt.span,
                  "expression statement has no expression");
    }
    auto lowered = lower_expr(*expr_stmt.expr);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ptr<hir_node>(
        make<hir_expr_stmt>(expr_stmt.span, std::move(*lowered)));
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = dynamic_cast<const ast::return_stmt &>(node);
    if (ret.value == nullptr) {
      return ptr<hir_node>(make<hir_return>(ret.span, nullptr));
    }
    auto value = lower_expr(*ret.value);
    if (!value.has_value()) {
      return std::unexpected(value.error());
    }
    return ptr<hir_node>(make<hir_return>(ret.span, std::move(*value)));
  }
  case ast::node_kind::if_stmt: {
    const auto &if_s = dynamic_cast<const ast::if_stmt &>(node);
    auto lowered =
        lower_if(if_s.branches, if_s.else_body, if_s.span, k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ptr<hir_node>(std::move(*lowered));
  }
  case ast::node_kind::match_stmt: {
    const auto &match_s = dynamic_cast<const ast::match_stmt &>(node);
    if (match_s.subject == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, match_s.span,
                  "match statement has no subject");
    }
    auto lowered = lower_match(*match_s.subject, match_s.arms, match_s.span,
                               k_unknown_type);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    return ptr<hir_node>(std::move(*lowered));
  }
  default:
    return fail(lowering_error_kind::unsupported_construct, node.span,
                std::format("statement kind {} is not lowered by the first "
                            "milestone (spec/typed-ir-design.md)",
                            static_cast<int>(node.kind)));
  }
}

auto lowerer::lower_if(const std::vector<ast::if_branch> &branches,
                       const std::vector<ast::ptr<ast::node>> &else_body,
                       source_span span, type_id type)
    -> std::expected<ptr<hir_if>, lowering_error> {
  if (branches.empty()) {
    return fail(lowering_error_kind::unsupported_construct, span,
                "if has no branches");
  }
  auto hir_branches = std::vector<hir_if_branch>{};
  hir_branches.reserve(branches.size());
  for (const auto &branch : branches) {
    if (branch.let_pattern != nullptr || branch.let_expr != nullptr) {
      return fail(lowering_error_kind::unsupported_construct, branch.span,
                  "`if let` is not lowered by the first milestone");
    }
    if (branch.condition == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, branch.span,
                  "if branch is missing a condition");
    }
    auto condition = lower_expr(*branch.condition);
    if (!condition.has_value()) {
      return std::unexpected(condition.error());
    }
    auto body = lower_block(branch.body, branch.span);
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    hir_branches.push_back(hir_if_branch{.condition = std::move(*condition),
                                         .body = std::move(*body)});
  }
  auto else_block = ptr<hir_block>{};
  if (!else_body.empty()) {
    auto lowered_else = lower_block(else_body, span);
    if (!lowered_else.has_value()) {
      return std::unexpected(lowered_else.error());
    }
    else_block = std::move(*lowered_else);
  }
  return make<hir_if>(span, type, std::move(hir_branches),
                      std::move(else_block));
}

auto lowerer::lower_pattern(const ast::node &pattern, symbol_id subject_symbol,
                            type_id subject_type,
                            std::vector<ptr<hir_node>> &pending)
    -> std::expected<ptr<hir_pattern>, lowering_error> {
  if (pattern.has_error) {
    return fail(lowering_error_kind::unsupported_construct, pattern.span,
                "pattern carries a parse/recovery error and cannot be lowered");
  }
  switch (pattern.kind) {
  case ast::node_kind::wildcard_pattern:
    return ptr<hir_pattern>(make<hir_wildcard_pattern>(pattern.span));
  case ast::node_kind::literal_pattern: {
    const auto &lit = dynamic_cast<const ast::literal_pattern &>(pattern);
    return ptr<hir_pattern>(
        make<hir_literal_pattern>(pattern.span, lit.lit_kind, lit.value));
  }
  case ast::node_kind::binding_pattern: {
    const auto &binding = dynamic_cast<const ast::binding_pattern &>(pattern);
    const auto symbol = declare_local(binding.name);
    pending.push_back(ptr<hir_node>(make<hir_let>(
        pattern.span, symbol, binding.name,
        make<hir_local_ref>(pattern.span, subject_type, subject_symbol,
                            std::string("<match subject>")))));
    return ptr<hir_pattern>(make<hir_wildcard_pattern>(pattern.span));
  }
  case ast::node_kind::group_pattern: {
    const auto &group = dynamic_cast<const ast::group_pattern &>(pattern);
    if (group.inner == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, pattern.span,
                  "grouped pattern has no inner pattern");
    }
    auto inner =
        lower_pattern(*group.inner, subject_symbol, subject_type, pending);
    if (!inner.has_value()) {
      return std::unexpected(inner.error());
    }
    if (group.alias.has_value()) {
      const auto symbol = declare_local(*group.alias);
      pending.push_back(ptr<hir_node>(make<hir_let>(
          pattern.span, symbol, *group.alias,
          make<hir_local_ref>(pattern.span, subject_type, subject_symbol,
                              std::string("<match subject>")))));
    }
    return inner;
  }
  case ast::node_kind::or_pattern: {
    const auto &alt = dynamic_cast<const ast::or_pattern &>(pattern);
    if (alt.alternatives.empty()) {
      return fail(lowering_error_kind::unsupported_construct, pattern.span,
                  "`|` pattern has no alternatives");
    }
    auto alternatives = ptr_vec<hir_pattern>{};
    alternatives.reserve(alt.alternatives.size());
    for (const auto &alternative : alt.alternatives) {
      if (alternative == nullptr) {
        return fail(lowering_error_kind::unsupported_construct, pattern.span,
                    "`|` pattern has a missing alternative");
      }
      auto local_pending = std::vector<ptr<hir_node>>{};
      auto lowered = lower_pattern(*alternative, subject_symbol, subject_type,
                                   local_pending);
      if (!lowered.has_value()) {
        return std::unexpected(lowered.error());
      }
      if (!local_pending.empty()) {
        return fail(lowering_error_kind::unsupported_construct,
                    alternative->span,
                    "a name bound inside one `|` alternative is not "
                    "supported by this milestone");
      }
      alternatives.push_back(std::move(*lowered));
    }
    return ptr<hir_pattern>(
        make<hir_or_pattern>(pattern.span, std::move(alternatives)));
  }
  default:
    return fail(lowering_error_kind::unsupported_construct, pattern.span,
                std::format("pattern kind {} is not lowered yet — structural/"
                            "payload destructuring needs projection "
                            "expressions this milestone doesn't have",
                            static_cast<int>(pattern.kind)));
  }
}

auto lowerer::lower_match(const ast::expr &subject_ast,
                          const std::vector<ast::match_arm> &arms,
                          source_span span, type_id type)
    -> std::expected<ptr<hir_match>, lowering_error> {
  auto subject_type = checked_type_of(subject_ast);
  if (!subject_type.has_value()) {
    return std::unexpected(subject_type.error());
  }
  auto subject = lower_expr(subject_ast);
  if (!subject.has_value()) {
    return std::unexpected(subject.error());
  }
  const auto subject_symbol = mint_symbol();

  auto hir_arms = std::vector<hir_match_arm>{};
  hir_arms.reserve(arms.size());
  for (const auto &arm : arms) {
    if (arm.has_error) {
      return fail(lowering_error_kind::unsupported_construct, arm.span,
                  "match arm carries a parse/recovery error and cannot be "
                  "lowered");
    }
    if (arm.pattern == nullptr) {
      return fail(lowering_error_kind::unsupported_construct, arm.span,
                  "match arm has no pattern");
    }
    push_scope();
    auto pending = std::vector<ptr<hir_node>>{};
    auto pattern =
        lower_pattern(*arm.pattern, subject_symbol, *subject_type, pending);
    if (!pattern.has_value()) {
      pop_scope();
      return std::unexpected(pattern.error());
    }

    auto guard = ptr<hir_expr>{};
    if (arm.guard != nullptr) {
      auto lowered_guard = lower_expr(*arm.guard);
      if (!lowered_guard.has_value()) {
        pop_scope();
        return std::unexpected(lowered_guard.error());
      }
      guard = std::move(*lowered_guard);
    }

    auto body = ptr<hir_block>{};
    if (arm.body_expr != nullptr) {
      auto value = lower_expr(*arm.body_expr);
      if (!value.has_value()) {
        pop_scope();
        return std::unexpected(value.error());
      }
      auto stmts = std::move(pending);
      stmts.push_back(ptr<hir_node>(
          make<hir_expr_stmt>(arm.body_expr->span, std::move(*value))));
      body = make<hir_block>(arm.span, k_unknown_type, std::move(stmts));
    } else {
      auto lowered_body = lower_block(arm.body_stmts, arm.span);
      if (!lowered_body.has_value()) {
        pop_scope();
        return std::unexpected(lowered_body.error());
      }
      body = std::move(*lowered_body);
      if (!pending.empty()) {
        auto merged = std::move(pending);
        for (auto &stmt_ptr : body->stmts) {
          merged.push_back(std::move(stmt_ptr));
        }
        body->stmts = std::move(merged);
      }
    }
    pop_scope();

    hir_arms.push_back(hir_match_arm{.pattern = std::move(*pattern),
                                     .guard = std::move(guard),
                                     .body = std::move(body)});
  }

  return make<hir_match>(span, type, std::move(*subject), subject_symbol,
                         std::move(hir_arms));
}

auto lowerer::lower_function(const ast::func_decl &decl)
    -> std::expected<ptr<hir_function>, lowering_error> {
  if (decl.has_error) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                "function declaration carries a parse/recovery error and "
                "cannot be lowered");
  }
  if (!decl.type_params.empty()) {
    return fail(lowering_error_kind::unsupported_construct, decl.span,
                "generic functions are not lowered until monomorphization "
                "exists (spec/typed-ir-design.md Decision 2, phase 5)");
  }
  if (decl.return_type == nullptr) {
    return fail(lowering_error_kind::missing_return_type, decl.span,
                std::format("function `{}` has no declared return type; the "
                            "first lowering milestone only lowers explicitly "
                            "annotated signatures",
                            decl.name));
  }

  push_scope();

  auto params = std::vector<hir_param>{};
  params.reserve(decl.params.size());
  for (const auto &param : decl.params) {
    if (param.type_annotation == nullptr) {
      pop_scope();
      return fail(lowering_error_kind::unannotated_parameter, param.span,
                  std::format("parameter in function `{}` has no explicit "
                              "type annotation; the first lowering milestone "
                              "only lowers explicitly annotated signatures",
                              decl.name));
    }
    if (param.default_value != nullptr) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, param.span,
                  "default parameter values are not lowered by the first "
                  "milestone");
    }
    if (param.pattern == nullptr || param.pattern->has_error ||
        param.pattern->kind != ast::node_kind::binding_pattern) {
      pop_scope();
      return fail(lowering_error_kind::unsupported_construct, param.span,
                  "only simple named parameters are lowered by the first "
                  "milestone (no destructuring patterns yet)");
    }
    auto type = checked_type_of(*param.pattern);
    if (!type.has_value()) {
      pop_scope();
      return std::unexpected(type.error());
    }
    const auto &binding =
        dynamic_cast<const ast::binding_pattern &>(*param.pattern);
    const auto symbol = declare_local(binding.name);
    params.push_back(
        hir_param{.symbol = symbol, .name = binding.name, .type = *type});
  }

  auto return_type = checked_type_of(*decl.return_type);
  if (!return_type.has_value()) {
    pop_scope();
    return std::unexpected(return_type.error());
  }

  auto body = std::expected<ptr<hir_block>, lowering_error>{};
  if (decl.body_expr != nullptr) {
    auto expr_type = checked_type_of(*decl.body_expr);
    if (!expr_type.has_value()) {
      pop_scope();
      return std::unexpected(expr_type.error());
    }
    auto value = lower_expr(*decl.body_expr);
    if (!value.has_value()) {
      pop_scope();
      return std::unexpected(value.error());
    }
    auto ret = ptr<hir_node>(
        make<hir_return>(decl.body_expr->span, std::move(*value)));
    auto stmts = ptr_vec<hir_node>{};
    stmts.push_back(std::move(ret));
    body =
        make<hir_block>(decl.body_expr->span, *return_type, std::move(stmts));
  } else {
    body = lower_block(decl.body_stmts, decl.span);
  }
  pop_scope();
  if (!body.has_value()) {
    return std::unexpected(body.error());
  }

  return make<hir_function>(decl.span, decl.name, std::move(params),
                            *return_type, std::move(*body));
}

} // namespace

auto lower_function(const ast::func_decl &decl,
                    const semantic::checked_types &checked)
    -> std::expected<ptr<hir_function>, lowering_error> {
  auto walker = lowerer(checked);
  return walker.lower_function(decl);
}

auto lower_module(const ast::file &file, std::string module_name,
                  const semantic::checked_types &checked)
    -> std::expected<ptr<hir_module>, lowering_error> {
  auto functions = ptr_vec<hir_function>{};
  for (const auto &item : file.items) {
    if (item == nullptr || item->kind != ast::node_kind::func_decl) {
      continue;
    }
    auto lowered =
        lower_function(dynamic_cast<const ast::func_decl &>(*item), checked);
    if (!lowered.has_value()) {
      return std::unexpected(lowered.error());
    }
    functions.push_back(std::move(*lowered));
  }
  return make<hir_module>(file.span, std::move(module_name),
                          std::move(functions));
}

} // namespace kira::hir
