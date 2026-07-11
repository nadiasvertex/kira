#include "src/comptime/hygiene.h"

#include <format>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace kira::comptime {

namespace {

/// Walks a quoted fragment once, renaming plain `let`/`var`/`where`-binding/
/// `crew` names to fresh synthetic ones and rewriting every in-fragment
/// reference to match — see `rename_internal_bindings`'s doc comment for
/// exactly what is and isn't covered.
class hygiene_renamer {
public:
  explicit hygiene_renamer(std::uint64_t &next_id) : next_id_(next_id) {}

  void run(ast::node &fragment) {
    scopes_.emplace_back();
    visit_node(fragment);
    scopes_.pop_back();
  }

private:
  std::uint64_t &next_id_;
  std::vector<std::unordered_map<std::string, std::string>> scopes_;

  auto fresh_name(const std::string &base) -> std::string {
    return std::format("{}$hygiene{}", base, next_id_++);
  }

  /// Registers `orig -> fresh` in the *current* (innermost) scope and
  /// returns the fresh name, so callers can assign it back onto the
  /// declaring node in one expression.
  auto declare(const std::string &orig) -> std::string {
    auto fresh = fresh_name(orig);
    scopes_.back()[orig] = fresh;
    return fresh;
  }

  /// Looks up `name` through the scope stack, innermost first; returns
  /// `nullptr` if `name` isn't a binding this pass renamed (a free
  /// reference, or a binding form this pass deliberately leaves alone).
  [[nodiscard]] auto lookup(const std::string &name) const
      -> const std::string * {
    for (const auto &scope : std::views::reverse(scopes_)) {
      if (const auto found = scope.find(name); found != scope.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  auto visit_body(std::vector<ast::ptr<ast::node>> &body) -> void {
    scopes_.emplace_back();
    for (auto &stmt : body) {
      if (stmt != nullptr) {
        visit_node(*stmt);
      }
    }
    scopes_.pop_back();
  }

  auto visit_node(ast::node &node) -> void {
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      auto &let = dynamic_cast<ast::let_stmt &>(node);
      if (let.initializer != nullptr) {
        visit_expr(*let.initializer);
      }
      if (let.pattern != nullptr &&
          let.pattern->kind == ast::node_kind::binding_pattern) {
        auto &binding = dynamic_cast<ast::binding_pattern &>(*let.pattern);
        binding.name = declare(binding.name);
      }
      visit_body(let.else_body);
      return;
    }
    case ast::node_kind::var_stmt: {
      auto &var = dynamic_cast<ast::var_stmt &>(node);
      if (var.initializer != nullptr) {
        visit_expr(*var.initializer);
      }
      var.name = declare(var.name);
      return;
    }
    case ast::node_kind::assign_stmt: {
      auto &assign = dynamic_cast<ast::assign_stmt &>(node);
      if (assign.target != nullptr) {
        visit_expr(*assign.target);
      }
      if (assign.value != nullptr) {
        visit_expr(*assign.value);
      }
      return;
    }
    case ast::node_kind::expr_stmt: {
      auto &stmt = dynamic_cast<ast::expr_stmt &>(node);
      if (stmt.expr != nullptr) {
        visit_expr(*stmt.expr);
      }
      return;
    }
    case ast::node_kind::return_stmt: {
      auto &stmt = dynamic_cast<ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        visit_expr(*stmt.value);
      }
      return;
    }
    case ast::node_kind::if_stmt: {
      auto &stmt = dynamic_cast<ast::if_stmt &>(node);
      visit_if_branches(stmt.branches, stmt.else_body);
      return;
    }
    case ast::node_kind::while_stmt: {
      auto &stmt = dynamic_cast<ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        visit_expr(*stmt.condition);
      }
      if (stmt.let_expr != nullptr) {
        visit_expr(*stmt.let_expr);
      }
      // `let_pattern` bindings (`while let`) are deliberately not renamed
      // — see the doc comment.
      visit_body(stmt.body);
      return;
    }
    case ast::node_kind::for_stmt: {
      auto &stmt = dynamic_cast<ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        visit_expr(*stmt.iterable);
      }
      if (stmt.guard != nullptr) {
        visit_expr(*stmt.guard);
      }
      // Loop patterns are deliberately not renamed — see the doc comment.
      visit_body(stmt.body);
      return;
    }
    case ast::node_kind::match_stmt: {
      auto &stmt = dynamic_cast<ast::match_stmt &>(node);
      if (stmt.subject != nullptr) {
        visit_expr(*stmt.subject);
      }
      for (auto &arm : stmt.arms) {
        visit_match_arm(arm);
      }
      return;
    }
    case ast::node_kind::crew_stmt: {
      auto &stmt = dynamic_cast<ast::crew_stmt &>(node);
      scopes_.emplace_back();
      stmt.name = declare(stmt.name);
      visit_body(stmt.body);
      scopes_.pop_back();
      return;
    }
    default:
      if (auto *as_expr = dynamic_cast<ast::expr *>(&node)) {
        visit_expr(*as_expr);
      }
      return;
    }
  }

  auto visit_if_branches(std::vector<ast::if_branch> &branches,
                         std::vector<ast::ptr<ast::node>> &else_body) -> void {
    for (auto &branch : branches) {
      if (branch.condition != nullptr) {
        visit_expr(*branch.condition);
      }
      if (branch.let_expr != nullptr) {
        visit_expr(*branch.let_expr);
      }
      // `let_pattern` bindings (`if let`/`elif let`) are deliberately not
      // renamed — see the doc comment.
      visit_body(branch.body);
    }
    visit_body(else_body);
  }

  auto visit_match_arm(ast::match_arm &arm) -> void {
    scopes_.emplace_back();
    // Arm patterns are deliberately not renamed — see the doc comment.
    if (arm.guard != nullptr) {
      visit_expr(*arm.guard);
    }
    if (arm.body_expr != nullptr) {
      visit_expr(*arm.body_expr);
    }
    for (auto &stmt : arm.body_stmts) {
      if (stmt != nullptr) {
        visit_node(*stmt);
      }
    }
    scopes_.pop_back();
  }

  auto visit_expr(ast::expr &expr) -> void {
    switch (expr.kind) {
    case ast::node_kind::ident_expr: {
      auto &ident = dynamic_cast<ast::ident_expr &>(expr);
      if (const auto *renamed = lookup(ident.name)) {
        ident.name = *renamed;
      }
      return;
    }
    case ast::node_kind::module_path_expr: {
      // The parser can't tell `a.b` field access from a module-qualified
      // path at parse time, so a renamed local read through this node
      // shape too — see `checker::infer_module_path`'s identical
      // "first segment might just be a value" handling.
      auto &path = dynamic_cast<ast::module_path_expr &>(expr);
      if (!path.segments.empty()) {
        if (const auto *renamed = lookup(path.segments.front())) {
          path.segments.front() = *renamed;
        }
      }
      return;
    }
    case ast::node_kind::binary_expr: {
      auto &bin = dynamic_cast<ast::binary_expr &>(expr);
      if (bin.lhs != nullptr) {
        visit_expr(*bin.lhs);
      }
      if (bin.rhs != nullptr) {
        visit_expr(*bin.rhs);
      }
      return;
    }
    case ast::node_kind::unary_expr: {
      auto &un = dynamic_cast<ast::unary_expr &>(expr);
      if (un.operand != nullptr) {
        visit_expr(*un.operand);
      }
      return;
    }
    case ast::node_kind::call_expr: {
      auto &call = dynamic_cast<ast::call_expr &>(expr);
      if (call.callee != nullptr) {
        visit_expr(*call.callee);
      }
      for (auto &arg : call.args) {
        if (arg.value != nullptr) {
          visit_expr(*arg.value);
        }
      }
      return;
    }
    case ast::node_kind::index_expr: {
      auto &idx = dynamic_cast<ast::index_expr &>(expr);
      if (idx.object != nullptr) {
        visit_expr(*idx.object);
      }
      if (idx.index != nullptr) {
        visit_expr(*idx.index);
      }
      return;
    }
    case ast::node_kind::field_expr: {
      auto &field = dynamic_cast<ast::field_expr &>(expr);
      if (field.object != nullptr) {
        visit_expr(*field.object);
      }
      return;
    }
    case ast::node_kind::cast_expr: {
      auto &cast = dynamic_cast<ast::cast_expr &>(expr);
      if (cast.operand != nullptr) {
        visit_expr(*cast.operand);
      }
      return;
    }
    case ast::node_kind::try_expr: {
      auto &try_ = dynamic_cast<ast::try_expr &>(expr);
      if (try_.operand != nullptr) {
        visit_expr(*try_.operand);
      }
      return;
    }
    case ast::node_kind::tuple_expr: {
      auto &tuple = dynamic_cast<ast::tuple_expr &>(expr);
      for (auto &elem : tuple.elements) {
        if (elem != nullptr) {
          visit_expr(*elem);
        }
      }
      return;
    }
    case ast::node_kind::array_expr: {
      auto &arr = dynamic_cast<ast::array_expr &>(expr);
      for (auto &elem : arr.elements) {
        if (elem != nullptr) {
          visit_expr(*elem);
        }
      }
      if (arr.fill_value != nullptr) {
        visit_expr(*arr.fill_value);
      }
      if (arr.fill_count != nullptr) {
        visit_expr(*arr.fill_count);
      }
      return;
    }
    case ast::node_kind::struct_expr: {
      auto &st = dynamic_cast<ast::struct_expr &>(expr);
      if (st.type_name != nullptr) {
        visit_expr(*st.type_name);
      }
      for (auto &field : st.fields) {
        // Shorthand `{x}` (`field.value == nullptr`) implicitly reads the
        // local `x` — not rewritten to the renamed name if `x` was
        // renamed, a documented gap (see the doc comment).
        if (field.value != nullptr) {
          visit_expr(*field.value);
        }
      }
      return;
    }
    case ast::node_kind::lambda_expr: {
      auto &lambda = dynamic_cast<ast::lambda_expr &>(expr);
      scopes_.emplace_back();
      // Parameters are deliberately not renamed — see the doc comment.
      if (lambda.body_expr != nullptr) {
        visit_expr(*lambda.body_expr);
      }
      for (auto &stmt : lambda.body_stmts) {
        if (stmt != nullptr) {
          visit_node(*stmt);
        }
      }
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::match_expr: {
      auto &match = dynamic_cast<ast::match_expr &>(expr);
      if (match.subject != nullptr) {
        visit_expr(*match.subject);
      }
      for (auto &arm : match.arms) {
        visit_match_arm(arm);
      }
      return;
    }
    case ast::node_kind::if_expr: {
      auto &if_e = dynamic_cast<ast::if_expr &>(expr);
      visit_if_branches(if_e.branches, if_e.else_body);
      return;
    }
    case ast::node_kind::for_expr: {
      auto &for_e = dynamic_cast<ast::for_expr &>(expr);
      scopes_.emplace_back();
      for (auto &clause : for_e.clauses) {
        if (clause.iterable != nullptr) {
          visit_expr(*clause.iterable);
        }
        // Comprehension patterns are deliberately not renamed.
      }
      if (for_e.guard != nullptr) {
        visit_expr(*for_e.guard);
      }
      if (for_e.yield_expr != nullptr) {
        visit_expr(*for_e.yield_expr);
      }
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::await_expr: {
      auto &await = dynamic_cast<ast::await_expr &>(expr);
      if (await.operand != nullptr) {
        visit_expr(*await.operand);
      }
      return;
    }
    case ast::node_kind::async_expr: {
      auto &async = dynamic_cast<ast::async_expr &>(expr);
      visit_body(async.body);
      return;
    }
    case ast::node_kind::par_expr: {
      auto &par = dynamic_cast<ast::par_expr &>(expr);
      for (auto &branch : par.branches) {
        if (branch != nullptr) {
          visit_expr(*branch);
        }
      }
      return;
    }
    case ast::node_kind::race_expr: {
      auto &race = dynamic_cast<ast::race_expr &>(expr);
      for (auto &branch : race.branches) {
        if (branch != nullptr) {
          visit_expr(*branch);
        }
      }
      return;
    }
    case ast::node_kind::on_expr: {
      auto &on = dynamic_cast<ast::on_expr &>(expr);
      if (on.sender != nullptr) {
        visit_expr(*on.sender);
      }
      visit_body(on.body);
      return;
    }
    case ast::node_kind::block_expr: {
      auto &block = dynamic_cast<ast::block_expr &>(expr);
      visit_body(block.stmts);
      return;
    }
    case ast::node_kind::static_expr: {
      auto &st = dynamic_cast<ast::static_expr &>(expr);
      if (st.operand != nullptr) {
        visit_expr(*st.operand);
      }
      return;
    }
    case ast::node_kind::group_expr: {
      auto &group = dynamic_cast<ast::group_expr &>(expr);
      if (group.inner != nullptr) {
        visit_expr(*group.inner);
      }
      return;
    }
    case ast::node_kind::where_expr: {
      auto &where = dynamic_cast<ast::where_expr &>(expr);
      scopes_.emplace_back();
      for (auto &binding : where.bindings) {
        // Not mutually recursive — each binding's own value is evaluated
        // against the *outer* scope, matching `where`'s own semantics
        // (see the struct's doc comment), so it's visited before this
        // binding's name is registered into the new frame.
        if (binding.value != nullptr) {
          visit_expr(*binding.value);
        }
        binding.name = declare(binding.name);
      }
      if (where.inner != nullptr) {
        visit_expr(*where.inner);
      }
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::interpolated_string_expr: {
      auto &interp = dynamic_cast<ast::interpolated_string_expr &>(expr);
      for (auto &segment : interp.segments) {
        if (!segment.is_literal && segment.value != nullptr) {
          visit_expr(*segment.value);
        }
      }
      return;
    }
    case ast::node_kind::quote_expr:
    case ast::node_kind::splice_expr:
      // Nested quotes/splices are left completely untouched — see the
      // doc comment ("no nested quotes").
      return;
    default:
      // literal_expr and anything else with no child expressions to
      // rewrite — nothing to do.
      return;
    }
  }
};

} // namespace

void rename_internal_bindings(ast::node &fragment, std::uint64_t &next_id) {
  hygiene_renamer(next_id).run(fragment);
}

} // namespace kira::comptime
