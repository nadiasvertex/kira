#include "src/comptime/hygiene.h"

#include <format>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/parser/ast_traversal.h"

namespace cinder::comptime {

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

  /// Fragment nodes this pass descends into: statements and expressions.
  /// Types and patterns are left alone — pattern bindings are deliberately
  /// not renamed (see the doc comment).
  static auto is_renamed_kind(ast::node_kind kind) -> bool {
    return ast::is_expr_kind(kind) || ast::is_stmt_kind(kind);
  }

  auto visit_children(ast::node &node) -> void {
    ast::for_each_child(node, [&](ast::node &child) -> void {
      if (is_renamed_kind(child.kind)) {
        visit_node(child);
      }
    });
  }

  auto visit_node(ast::node &node) -> void {
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      auto &let = ast::cast<ast::let_stmt>(node);
      if (let.initializer != nullptr) {
        visit_node(*let.initializer);
      }
      if (let.pattern != nullptr &&
          let.pattern->kind == ast::node_kind::binding_pattern) {
        auto &binding = ast::cast<ast::binding_pattern>(*let.pattern);
        binding.name = declare(binding.name);
      }
      visit_body(let.else_body);
      return;
    }
    case ast::node_kind::var_stmt: {
      auto &var = ast::cast<ast::var_stmt>(node);
      if (var.initializer != nullptr) {
        visit_node(*var.initializer);
      }
      var.name = declare(var.name);
      return;
    }
    case ast::node_kind::if_stmt: {
      auto &stmt = ast::cast<ast::if_stmt>(node);
      visit_if_branches(stmt.branches, stmt.else_body);
      return;
    }
    case ast::node_kind::if_expr: {
      auto &if_e = ast::cast<ast::if_expr>(node);
      visit_if_branches(if_e.branches, if_e.else_body);
      return;
    }
    case ast::node_kind::while_stmt: {
      auto &stmt = ast::cast<ast::while_stmt>(node);
      if (stmt.condition != nullptr) {
        visit_node(*stmt.condition);
      }
      if (stmt.let_expr != nullptr) {
        visit_node(*stmt.let_expr);
      }
      visit_body(stmt.body);
      return;
    }
    case ast::node_kind::scope_stmt:
      visit_body(ast::cast<ast::scope_stmt>(node).body);
      return;
    case ast::node_kind::for_stmt: {
      auto &stmt = ast::cast<ast::for_stmt>(node);
      if (stmt.iterable != nullptr) {
        visit_node(*stmt.iterable);
      }
      if (stmt.guard != nullptr) {
        visit_node(*stmt.guard);
      }
      visit_body(stmt.body);
      return;
    }
    case ast::node_kind::match_stmt: {
      auto &stmt = ast::cast<ast::match_stmt>(node);
      if (stmt.subject != nullptr) {
        visit_node(*stmt.subject);
      }
      for (auto &arm : stmt.arms) {
        visit_match_arm(arm);
      }
      return;
    }
    case ast::node_kind::match_expr: {
      auto &match = ast::cast<ast::match_expr>(node);
      if (match.subject != nullptr) {
        visit_node(*match.subject);
      }
      for (auto &arm : match.arms) {
        visit_match_arm(arm);
      }
      return;
    }
    case ast::node_kind::crew_stmt: {
      auto &stmt = ast::cast<ast::crew_stmt>(node);
      scopes_.emplace_back();
      stmt.name = declare(stmt.name);
      visit_body(stmt.body);
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::ident_expr: {
      auto &ident = ast::cast<ast::ident_expr>(node);
      if (const auto *renamed = lookup(ident.name)) {
        ident.name = *renamed;
      }
      return;
    }
    case ast::node_kind::module_path_expr: {
      // `temp.x` with `temp` bound by this fragment. No module-vs-value
      // decision is made here: `lookup` only finds the fragment's own
      // bindings, and a lexical binding always wins (spec "Dotted Names",
      // rule 1), so the checker will read this root as that binding whatever
      // it is called — renaming it with its binding is all hygiene needs. A
      // root the fragment doesn't bind is left alone for the checker.
      auto &path = ast::cast<ast::module_path_expr>(node);
      if (!path.segments.empty()) {
        if (const auto *renamed = lookup(path.segments.front())) {
          path.segments.front() = *renamed;
        }
      }
      return;
    }
    case ast::node_kind::lambda_expr: {
      auto &lambda = ast::cast<ast::lambda_expr>(node);
      scopes_.emplace_back();
      // Parameters are deliberately not renamed — see the doc comment.
      if (lambda.body_expr != nullptr) {
        visit_node(*lambda.body_expr);
      }
      visit_stmts(lambda.body_stmts);
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::for_expr: {
      auto &for_e = ast::cast<ast::for_expr>(node);
      scopes_.emplace_back();
      for (auto &clause : for_e.clauses) {
        if (clause.iterable != nullptr) {
          visit_node(*clause.iterable);
        }
        // Comprehension patterns are deliberately not renamed.
      }
      if (for_e.guard != nullptr) {
        visit_node(*for_e.guard);
      }
      if (for_e.yield_expr != nullptr) {
        visit_node(*for_e.yield_expr);
      }
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::async_expr:
      visit_body(ast::cast<ast::async_expr>(node).body);
      return;
    case ast::node_kind::on_expr: {
      auto &on = ast::cast<ast::on_expr>(node);
      if (on.sender != nullptr) {
        visit_node(*on.sender);
      }
      visit_body(on.body);
      return;
    }
    case ast::node_kind::block_expr:
      visit_body(ast::cast<ast::block_expr>(node).stmts);
      return;
    case ast::node_kind::where_expr: {
      auto &where = ast::cast<ast::where_expr>(node);
      scopes_.emplace_back();
      for (auto &binding : where.bindings) {
        // Not mutually recursive — each binding's own value is evaluated
        // against the *outer* scope, matching `where`'s own semantics
        // (see the struct's doc comment), so it's visited before this
        // binding's name is registered into the new frame.
        if (binding.value != nullptr) {
          visit_node(*binding.value);
        }
        binding.name = declare(binding.name);
      }
      if (where.inner != nullptr) {
        visit_node(*where.inner);
      }
      scopes_.pop_back();
      return;
    }
    case ast::node_kind::quote_expr:
    case ast::node_kind::splice_expr:
    case ast::node_kind::splice_stmt:
      // Nested quotes/splices are left completely untouched — see the
      // doc comment ("no nested quotes").
      return;
    default:
      // Everything else renames through its statement and expression
      // children. Struct shorthand `{x}` (`field.value == nullptr`)
      // implicitly reads the local `x` and is not rewritten, a documented
      // gap (see the doc comment). Nested items (`def`, `type`, ...) are
      // not fragment code and are left alone.
      if (is_renamed_kind(node.kind)) {
        visit_children(node);
      }
      return;
    }
  }

  auto visit_stmts(std::vector<ast::ptr<ast::node>> &stmts) -> void {
    for (auto &stmt : stmts) {
      if (stmt != nullptr) {
        visit_node(*stmt);
      }
    }
  }

  auto visit_if_branches(std::vector<ast::if_branch> &branches,
                         std::vector<ast::ptr<ast::node>> &else_body) -> void {
    for (auto &branch : branches) {
      if (branch.condition != nullptr) {
        visit_node(*branch.condition);
      }
      if (branch.let_expr != nullptr) {
        visit_node(*branch.let_expr);
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
      visit_node(*arm.guard);
    }
    if (arm.body_expr != nullptr) {
      visit_node(*arm.body_expr);
    }
    visit_stmts(arm.body_stmts);
    scopes_.pop_back();
  }
};

} // namespace

void rename_internal_bindings(ast::node &fragment, std::uint64_t &next_id) {
  hygiene_renamer(next_id).run(fragment);
}

} // namespace cinder::comptime
