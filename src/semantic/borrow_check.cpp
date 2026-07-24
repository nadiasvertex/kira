#include "borrow_check.h"

#include <format>
#include <string>
#include <vector>

namespace kira::semantic {
namespace {

/// Peels enclosing parentheses off an expression. `f((&x))` lends `x` exactly
/// like `f(&x)` does, so a borrow wrapped in one or more `group_expr`s is
/// still the borrow it wraps.
auto strip_groups(const ast::expr &expr) -> const ast::expr & {
  const auto *current = &expr;
  while (current->kind == ast::node_kind::group_expr) {
    const auto &group = dynamic_cast<const ast::group_expr &>(*current);
    if (group.inner == nullptr) {
      break;
    }
    current = group.inner.get();
  }
  return *current;
}

/// The root binding a place expression ultimately projects out of — the
/// `x` in `x`, `x.field`, `x[i]`, `(x).field`, and so on. Mirrors
/// `checker::assignment_root_ident` (`check.cpp`), which this pass cannot
/// reach directly. Returns an empty string for anything not rooted in a
/// plain binding (a deref, a call result, a literal, ...).
auto root_binding_name(const ast::expr &expr) -> std::string {
  const auto *current = &expr;
  while (true) {
    switch (current->kind) {
    case ast::node_kind::ident_expr:
      return dynamic_cast<const ast::ident_expr &>(*current).name;
    case ast::node_kind::field_expr: {
      const auto &field = dynamic_cast<const ast::field_expr &>(*current);
      if (field.object == nullptr) {
        return {};
      }
      current = field.object.get();
      break;
    }
    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(*current);
      if (index.object == nullptr) {
        return {};
      }
      current = index.object.get();
      break;
    }
    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(*current);
      if (group.inner == nullptr) {
        return {};
      }
      current = group.inner.get();
      break;
    }
    default:
      return {};
    }
  }
}

/// A borrow expression collected from a call's argument list, for the
/// exclusivity check.
struct call_borrow {
  std::string root;    ///< Root binding being borrowed (may be empty).
  bool is_mut = false; ///< `&mut` vs `&`.
  source_span span = source_span::dummy();
};

/// Walks every checked function and lambda body enforcing the borrow
/// discipline — see `borrow_check.h` for the rules. Holds no per-binding
/// state: unlike the move checker, nothing needs tracking across statements,
/// because a borrow can never be stored, so the analysis is a pure
/// type-directed tree walk.
class borrow_checker {
public:
  borrow_checker(const checked_types &checked, diagnostic_bag &diag,
                 file_id_type file_id)
      : checked_(checked), diag_(diag), file_id_(file_id) {}

  auto check_function(const ast::func_decl &decl) -> void {
    if (decl.body_expr != nullptr) {
      walk_expr(*decl.body_expr, /*borrow_ok=*/false);
    }
    walk_body(decl.body_stmts);
  }

  auto check_lambda(const ast::lambda_expr &lambda) -> void {
    if (lambda.body_expr != nullptr) {
      walk_expr(*lambda.body_expr, /*borrow_ok=*/false);
    }
    walk_body(lambda.body_stmts);
  }

private:
  const checked_types &checked_;
  diagnostic_bag &diag_;
  file_id_type file_id_;

  auto lookup_type(const ast::node *node) const -> type_id {
    if (node == nullptr) {
      return k_unknown_type;
    }
    if (const auto it = checked_.node_types.find(node);
        it != checked_.node_types.end()) {
      return it->second;
    }
    return k_unknown_type;
  }

  /// Whether `id` is a view type (`slice[T]` / `slice_mut[T]`) — the one
  /// borrowing value the language lets outlive a single call.
  [[nodiscard]] auto is_view_type(type_id id) const -> bool {
    const auto &entry = checked_.types.entry(id);
    return entry.kind == type_kind::builtin_generic_kind &&
           (entry.name == "slice" || entry.name == "slice_mut");
  }

  /// Whether `unary` is a plain-reference borrow — an `&`/`&mut` whose result
  /// is a `ref_kind` referring to an ordinary value. A `&`/`&mut
  /// container[a..b]` borrows a *view* instead: `infer_unary` (`check.cpp`)
  /// types it as a reference to a `slice`/`slice_mut`, and views are exempt
  /// from the escape rule (they are allowed to outlive the call — see the Views
  /// chapter), so a borrow whose referent is a view is not a plain-reference
  /// borrow here.
  [[nodiscard]] auto is_ref_borrow(const ast::unary_expr &unary) const -> bool {
    if (unary.op != ast::unary_op::addr_of &&
        unary.op != ast::unary_op::addr_of_mut) {
      return false;
    }
    const auto type = lookup_type(&unary);
    if (checked_.types.is_unknown(type)) {
      return false;
    }
    const auto &entry = checked_.types.entry(type);
    if (entry.kind != type_kind::ref_kind) {
      return false;
    }
    return !is_view_type(entry.result);
  }

  /// If `expr` (ignoring parentheses) is a plain-reference borrow, returns it;
  /// otherwise `nullptr`.
  [[nodiscard]] auto as_ref_borrow(const ast::expr &expr) const
      -> const ast::unary_expr * {
    const auto &stripped = strip_groups(expr);
    if (stripped.kind != ast::node_kind::unary_expr) {
      return nullptr;
    }
    const auto &unary = dynamic_cast<const ast::unary_expr &>(stripped);
    return is_ref_borrow(unary) ? &unary : nullptr;
  }

  /// Walks `expr`. `borrow_ok` is true only in a *passing* position — a
  /// direct call argument, the callee, or the base of a field/index
  /// projection — where a plain-reference borrow lends its operand rather
  /// than escaping. A borrow found with `borrow_ok` false is stored, and
  /// rejected.
  auto walk_expr(const ast::expr &expr, bool borrow_ok) -> void {
    if (expr.has_error) {
      return;
    }
    switch (expr.kind) {
    case ast::node_kind::unary_expr: {
      const auto &unary = dynamic_cast<const ast::unary_expr &>(expr);
      if (is_ref_borrow(unary) && !borrow_ok) {
        report_escape(unary);
      }
      // A borrow's operand, a deref's pointee, any unary operand: never a
      // passing position of its own.
      if (unary.operand != nullptr) {
        walk_expr(*unary.operand, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::group_expr: {
      const auto &group = dynamic_cast<const ast::group_expr &>(expr);
      if (group.inner != nullptr) {
        walk_expr(*group.inner, borrow_ok);
      }
      return;
    }

    case ast::node_kind::field_expr: {
      // Projecting through a borrow (`(&x).field`) passes it through for the
      // access; it is not stored, so a borrow as the base is allowed.
      const auto &field = dynamic_cast<const ast::field_expr &>(expr);
      if (field.object != nullptr) {
        walk_expr(*field.object, /*borrow_ok=*/true);
      }
      return;
    }

    case ast::node_kind::index_expr: {
      const auto &index = dynamic_cast<const ast::index_expr &>(expr);
      if (index.object != nullptr) {
        walk_expr(*index.object, /*borrow_ok=*/true);
      }
      if (index.index != nullptr) {
        walk_expr(*index.index, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::call_expr: {
      const auto &call = dynamic_cast<const ast::call_expr &>(expr);
      if (call.callee != nullptr) {
        walk_expr(*call.callee, /*borrow_ok=*/true);
      }
      for (const auto &arg : call.args) {
        if (arg.value != nullptr) {
          walk_expr(*arg.value, /*borrow_ok=*/true);
        }
      }
      check_call_exclusivity(call);
      return;
    }

    case ast::node_kind::binary_expr: {
      const auto &binary = dynamic_cast<const ast::binary_expr &>(expr);
      if (binary.lhs != nullptr) {
        walk_expr(*binary.lhs, /*borrow_ok=*/false);
      }
      if (binary.rhs != nullptr) {
        walk_expr(*binary.rhs, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::cast_expr: {
      const auto &cast = dynamic_cast<const ast::cast_expr &>(expr);
      if (cast.operand != nullptr) {
        walk_expr(*cast.operand, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::try_expr: {
      const auto &tri = dynamic_cast<const ast::try_expr &>(expr);
      if (tri.operand != nullptr) {
        walk_expr(*tri.operand, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::tuple_expr: {
      const auto &tuple = dynamic_cast<const ast::tuple_expr &>(expr);
      for (const auto &element : tuple.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*borrow_ok=*/false);
        }
      }
      return;
    }

    case ast::node_kind::array_expr: {
      const auto &array = dynamic_cast<const ast::array_expr &>(expr);
      for (const auto &element : array.elements) {
        if (element != nullptr) {
          walk_expr(*element, /*borrow_ok=*/false);
        }
      }
      if (array.fill_value != nullptr) {
        walk_expr(*array.fill_value, /*borrow_ok=*/false);
      }
      if (array.fill_count != nullptr) {
        walk_expr(*array.fill_count, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::struct_expr: {
      const auto &literal = dynamic_cast<const ast::struct_expr &>(expr);
      if (literal.type_name != nullptr) {
        walk_expr(*literal.type_name, /*borrow_ok=*/false);
      }
      for (const auto &field : literal.fields) {
        if (field.value != nullptr) {
          walk_expr(*field.value, /*borrow_ok=*/false);
        }
      }
      return;
    }

    case ast::node_kind::lambda_expr:
      borrow_checker(checked_, diag_, file_id_)
          .check_lambda(dynamic_cast<const ast::lambda_expr &>(expr));
      return;

    case ast::node_kind::if_expr: {
      const auto &if_e = dynamic_cast<const ast::if_expr &>(expr);
      walk_if_branches(if_e.branches, if_e.else_body);
      return;
    }

    case ast::node_kind::match_expr: {
      const auto &match_e = dynamic_cast<const ast::match_expr &>(expr);
      if (match_e.subject != nullptr) {
        walk_expr(*match_e.subject, /*borrow_ok=*/false);
      }
      walk_match_arms(match_e.arms);
      return;
    }

    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(expr);
      walk_body(block.stmts);
      return;
    }

    default:
      // Identifiers, literals, module paths, and constructs with no
      // sub-expression of interest: nothing to check.
      return;
    }
  }

  /// The four aliasing rules reduce to a single per-call check: gather the
  /// explicit plain-reference borrows in this call's argument list, then
  /// reject any pair over the same root binding where at least one is `&mut`
  /// (two `&mut`, or a `&mut` and a `&` — any number of plain `&` is fine).
  auto check_call_exclusivity(const ast::call_expr &call) -> void {
    auto borrows = std::vector<call_borrow>{};
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        continue;
      }
      if (const auto *borrow = as_ref_borrow(*arg.value)) {
        auto root = borrow->operand != nullptr
                        ? root_binding_name(*borrow->operand)
                        : std::string{};
        if (root.empty()) {
          // A borrow of something not rooted in a plain binding (a temporary,
          // a call result) can't alias another named place here.
          continue;
        }
        borrows.push_back(call_borrow{
            .root = std::move(root),
            .is_mut = borrow->op == ast::unary_op::addr_of_mut,
            .span = borrow->span,
        });
      }
    }

    for (size_t j = 1; j < borrows.size(); ++j) {
      for (size_t i = 0; i < j; ++i) {
        if (borrows[i].root != borrows[j].root) {
          continue;
        }
        if (!borrows[i].is_mut && !borrows[j].is_mut) {
          continue; // two shared borrows never conflict
        }
        report_exclusivity(borrows[i], borrows[j]);
        break; // one diagnostic per later borrow is enough
      }
    }
  }

  auto walk_body(const std::vector<ast::ptr<ast::node>> &stmts) -> void {
    for (const auto &stmt : stmts) {
      if (stmt != nullptr) {
        walk_stmt(*stmt);
      }
    }
  }

  auto walk_stmt(const ast::node &node) -> void {
    if (node.has_error) {
      return;
    }
    switch (node.kind) {
    case ast::node_kind::let_stmt: {
      const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*borrow_ok=*/false);
      }
      walk_body(stmt.else_body);
      return;
    }

    case ast::node_kind::var_stmt: {
      const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
      if (stmt.initializer != nullptr) {
        walk_expr(*stmt.initializer, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::assign_stmt: {
      const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*borrow_ok=*/false);
      }
      if (stmt.target != nullptr) {
        walk_expr(*stmt.target, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::expr_stmt: {
      const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
      if (stmt.expr != nullptr) {
        walk_expr(*stmt.expr, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::return_stmt: {
      const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
      if (stmt.value != nullptr) {
        walk_expr(*stmt.value, /*borrow_ok=*/false);
      }
      return;
    }

    case ast::node_kind::break_stmt:
    case ast::node_kind::continue_stmt:
      return;

    case ast::node_kind::if_stmt: {
      const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
      walk_if_branches(stmt.branches, stmt.else_body);
      return;
    }

    case ast::node_kind::while_stmt: {
      const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
      if (stmt.condition != nullptr) {
        walk_expr(*stmt.condition, /*borrow_ok=*/false);
      }
      if (stmt.let_expr != nullptr) {
        walk_expr(*stmt.let_expr, /*borrow_ok=*/false);
      }
      walk_body(stmt.body);
      return;
    }

    case ast::node_kind::for_stmt: {
      const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
      if (stmt.iterable != nullptr) {
        walk_expr(*stmt.iterable, /*borrow_ok=*/false);
      }
      if (stmt.guard != nullptr) {
        walk_expr(*stmt.guard, /*borrow_ok=*/false);
      }
      walk_body(stmt.body);
      return;
    }

    case ast::node_kind::match_stmt: {
      const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
      if (stmt.subject != nullptr) {
        walk_expr(*stmt.subject, /*borrow_ok=*/false);
      }
      walk_match_arms(stmt.arms);
      return;
    }

    case ast::node_kind::block_expr: {
      const auto &block = dynamic_cast<const ast::block_expr &>(node);
      walk_body(block.stmts);
      return;
    }

    default:
      if (const auto *expr = dynamic_cast<const ast::expr *>(&node)) {
        walk_expr(*expr, /*borrow_ok=*/false);
      }
      return;
    }
  }

  auto walk_if_branches(const std::vector<ast::if_branch> &branches,
                        const std::vector<ast::ptr<ast::node>> &else_body)
      -> void {
    for (const auto &branch : branches) {
      if (branch.condition != nullptr) {
        walk_expr(*branch.condition, /*borrow_ok=*/false);
      }
      if (branch.let_expr != nullptr) {
        walk_expr(*branch.let_expr, /*borrow_ok=*/false);
      }
      walk_body(branch.body);
    }
    walk_body(else_body);
  }

  auto walk_match_arms(const std::vector<ast::match_arm> &arms) -> void {
    for (const auto &arm : arms) {
      if (arm.guard != nullptr) {
        walk_expr(*arm.guard, /*borrow_ok=*/false);
      }
      if (arm.body_expr != nullptr) {
        walk_expr(*arm.body_expr, /*borrow_ok=*/false);
      }
      walk_body(arm.body_stmts);
    }
  }

  auto report_escape(const ast::unary_expr &borrow) -> void {
    const auto *spelling =
        borrow.op == ast::unary_op::addr_of_mut ? "&mut" : "&";
    const auto root = borrow.operand != nullptr
                          ? root_binding_name(*borrow.operand)
                          : std::string{};
    const auto subject =
        root.empty() ? std::string("value") : std::format("`{}`", root);
    auto d = diagnostic(
        diagnostic_level::error,
        std::format("a borrow of {} cannot escape the call it was made for",
                    subject),
        file_id_);
    d.with_label(borrow.span,
                 std::format("this `{}` borrow is created here", spelling));
    d.with_note("a borrow lends a value to a function for the duration of one "
                "call; it is a way of passing a value, not a value in its own "
                "right, so it cannot be stored in a binding, returned, or "
                "placed in a struct field or collection");
    d.with_help("pass the borrow directly to the call that needs it; to hand "
                "back a window into a collection that outlives the call, "
                "return a view instead (see the Views chapter), or copy the "
                "value if the callee needs its own");
    diag_.emit(d);
  }

  auto report_exclusivity(const call_borrow &earlier, const call_borrow &later)
      -> void {
    const auto both_mut = earlier.is_mut && later.is_mut;
    auto message =
        both_mut
            ? std::format("cannot borrow `{}` as mutable more than once in the "
                          "same call",
                          later.root)
            : std::format("cannot borrow `{}` as mutable and immutable at the "
                          "same time",
                          later.root);
    auto d = diagnostic(diagnostic_level::error, std::move(message), file_id_);
    d.with_label(
        later.span,
        later.is_mut
            ? std::format("`{}` borrowed as mutable here", later.root)
            : std::format("`{}` borrowed as immutable here", later.root));
    d.with_secondary_label(
        earlier.span,
        earlier.is_mut
            ? std::format("`{}` already borrowed as mutable here", earlier.root)
            : std::format("`{}` already borrowed as immutable here",
                          earlier.root));
    d.with_note("at most one `&mut` borrow of a value may exist at a time, and "
                "a `&mut` borrow cannot coexist with any `&` borrow; any "
                "number of `&` borrows are fine");
    d.with_note("borrows are compared at whole-variable granularity, so two "
                "borrows of different fields of the same variable also "
                "conflict here");
    d.with_help(std::format(
        "pass a single borrow of `{0}` to this call, or copy `{0}` so each "
        "call receives its own value",
        later.root));
    diag_.emit(d);
  }
};

/// Recursively finds every `func_decl` reachable from `item` (including
/// through `impl`/`trait`/`extend`/nested-`module` bodies) and runs the
/// borrow checker over its body. Mirrors `walk_item` in `move_check.cpp`.
auto walk_item(const ast::node &item, const checked_types &checked,
               diagnostic_bag &diag, file_id_type file_id) -> void {
  switch (item.kind) {
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(item);
    if (decl.modifiers.is_intrinsic) {
      return;
    }
    borrow_checker(checked, diag, file_id).check_function(decl);
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &decl = dynamic_cast<const ast::impl_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::extend_decl: {
    const auto &decl = dynamic_cast<const ast::extend_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  case ast::node_kind::sub_module_decl: {
    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(item);
    for (const auto &child : decl.items) {
      if (child != nullptr) {
        walk_item(*child, checked, diag, file_id);
      }
    }
    return;
  }
  default:
    return;
  }
}

} // namespace

auto check_borrows(const std::vector<parsed_module> &inputs,
                   const checked_types &checked, diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors,
                   unsigned skip_from_fileid) -> void {
  for (const auto &input : inputs) {
    if (static_cast<unsigned>(input.file_id) >= skip_from_fileid) {
      continue;
    }
    if (input.ast_file == nullptr) {
      continue;
    }
    if (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }
    for (const auto &item : input.ast_file->items) {
      if (item != nullptr) {
        walk_item(*item, checked, diag, input.file_id);
      }
    }
  }
}

} // namespace kira::semantic
