#include "session.h"

#include <format>
#include <ranges>
#include <utility>

#include "src/semantic/binding_walk.h"
#include "src/semantic/module_index.h"
#include "src/semantic/symbols.h"

namespace cinder::semantic {
namespace {

/// One name a pattern would bind, collected before the enclosing scope that
/// should own it exists yet.
struct pattern_binding_spec {
  std::string name;
  source_location location;
};

/// Ambient state threaded through the AST walk: the session being built, the
/// file currently being walked, and its fully-qualified module name.
struct scope_build_context {
  semantic_session &session;
  file_id_type file_id = 0;
  std::string module_name;
};

/// Appends a new scope to `session` and returns its id.
auto add_scope(semantic_session &session, semantic_scope_kind kind,
               scope_id parent, file_id_type file_id,
               std::string_view module_name, std::string_view debug_name,
               source_location location) -> scope_id {
  const auto id = static_cast<scope_id>(session.scopes.size());
  session.scopes.push_back(semantic_scope{
      .id = id,
      .kind = kind,
      .parent = parent,
      .file_id = file_id,
      .module_name = std::string(module_name),
      .debug_name = std::string(debug_name),
      .location = location,
      .symbols = {},
  });
  return id;
}

/// Appends a new symbol to `session`, registers it in `defining_scope`'s
/// symbol list, and returns its id.
auto add_symbol(semantic_session &session, scope_id defining_scope,
                const semantic_symbol_spec &spec) -> symbol_id {
  const auto id = static_cast<symbol_id>(session.symbols.size());
  session.symbols.push_back(semantic_symbol{
      .id = id,
      .name = spec.name,
      .kind = spec.kind,
      .name_space = spec.name_space,
      .kind_name = std::string(semantic_symbol_kind_name(spec.kind)),
      .visibility = spec.visibility,
      .location = spec.location,
      .defining_scope = defining_scope,
  });
  if (const auto *scope = find_semantic_scope(session, defining_scope);
      scope != nullptr) {
    session.scopes[defining_scope].symbols.push_back(id);
  }
  return id;
}

/// Records which scope was active at `node`, for later lookup by
/// `find_node_scope`. A no-op for a null node (recovery placeholders).
auto record_node_scope(semantic_session &session, const ast::node *node,
                       scope_id scope) -> void {
  if (node == nullptr) {
    return;
  }
  session.node_scopes.emplace(node, scope);
}

/// Collects every name a pattern would bind, in the order the pattern would
/// bind them, via the shared `cinder::semantic::collect_pattern_bindings`
/// (`binding_walk.h`) — appending each as a `pattern_binding_spec` tagged
/// with `file_id` so callers here don't need to carry it separately.
auto collect_pattern_bindings(const ast::pattern &pattern, file_id_type file_id,
                              std::vector<pattern_binding_spec> &out) -> void {
  for (const auto &binding :
       cinder::semantic::collect_pattern_bindings(pattern)) {
    out.push_back(pattern_binding_spec{
        .name = binding.name,
        .location =
            source_location{
                .file_id = file_id,
                .span = binding.span,
            },
    });
  }
}

/// Creates a fresh scope holding `bindings` as symbols of `binding_kind`, so
/// they shadow the parent scope from this point in a block onward (e.g. the
/// scope a `let` introduces for the statements that follow it). Returns
/// `parent_scope` unchanged when there are no bindings to add.
auto extend_scope_with_bindings(
    semantic_session &session, scope_id parent_scope, semantic_scope_kind kind,
    std::string_view debug_name, file_id_type file_id,
    std::string_view module_name,
    const std::vector<pattern_binding_spec> &bindings,
    semantic_symbol_kind binding_kind) -> scope_id {
  if (bindings.empty()) {
    return parent_scope;
  }

  auto extended_scope =
      add_scope(session, kind, parent_scope, file_id, module_name, debug_name,
                bindings.front().location);
  for (const auto &binding : bindings) {
    add_symbol(session, extended_scope,
               semantic_symbol_spec{
                   .name = binding.name,
                   .kind = binding_kind,
                   .name_space = symbol_namespace::value_namespace,
                   .visibility = ast::visibility::def,
                   .location = binding.location,
               });
  }
  return extended_scope;
}

/// Forward declaration: walks one AST node, building scopes/symbols beneath
/// `active_scope`, and returns the scope subsequent siblings should use (this
/// is how a `let` in a block extends the scope for the statements after it).
auto walk_node(const ast::node &node, scope_id active_scope,
               const scope_build_context &context) -> scope_id;

/// Forward declaration: walks one `static` declaration — see its definition.
auto walk_static_decl(const ast::static_decl &decl, scope_id active_scope,
                      const scope_build_context &context) -> scope_id;

/// Walks each item in `items` in order, threading the scope returned by one
/// node's walk into the next (so sequential `let`/`var` bindings accumulate).
auto walk_node_list(const std::vector<ast::ptr<ast::node>> &items,
                    scope_id active_scope, const scope_build_context &context)
    -> scope_id {
  auto current_scope = active_scope;
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }
    current_scope = walk_node(*item, current_scope, context);
  }
  return current_scope;
}

/// Convenience wrapper adding a scope located in the file/module of `context`.
auto create_block_scope(scope_id parent_scope,
                        const scope_build_context &context,
                        semantic_scope_kind kind, std::string_view debug_name,
                        source_span span) -> scope_id {
  return add_scope(context.session, kind, parent_scope, context.file_id,
                   context.module_name, debug_name,
                   source_location{
                       .file_id = context.file_id,
                       .span = span,
                   });
}

/// Walks `child` beneath `scope` when present.
template <typename node_type>
auto walk_child(const ast::ptr<node_type> &child, scope_id scope,
                const scope_build_context &context) -> void {
  if (child != nullptr) {
    walk_node(*child, scope, context);
  }
}

/// Walks each of `children` beneath `scope`. Unlike `walk_node_list`, no
/// child's bindings reach its siblings: these are expressions, patterns, or
/// types, not statements.
template <typename node_type>
auto walk_children(const std::vector<ast::ptr<node_type>> &children,
                   scope_id scope, const scope_build_context &context) -> void {
  for (const auto &child : children) {
    walk_child(child, scope, context);
  }
}

/// Walks the argument payloads of a type/functor application (a value
/// argument is an expression; a type argument is a type).
auto walk_type_args(const std::vector<ast::type_arg> &args, scope_id scope,
                    const scope_build_context &context) -> void {
  for (const auto &arg : args) {
    walk_child(arg.value, scope, context);
  }
}

/// Walks the bound or value type of each generic parameter.
auto walk_type_param_bounds(const std::vector<ast::type_param> &params,
                            scope_id scope, const scope_build_context &context)
    -> void {
  for (const auto &param : params) {
    walk_child(param.bound_or_type, scope, context);
  }
}

/// Walks every term of a `+`-joined bound.
auto walk_bound(const ast::bound &bound, scope_id scope,
                const scope_build_context &context) -> void {
  for (const auto &term : bound.terms) {
    walk_child(term.type, scope, context);
  }
}

/// Whether `kind` is a scope whose declarations the module-symbol pass
/// (`module_symbol_spec`) registers before any body is walked, so a nested
/// walk must not register them again.
auto is_item_scope(semantic_scope_kind kind) -> bool {
  return kind == semantic_scope_kind::module_scope ||
         kind == semantic_scope_kind::trait_scope ||
         kind == semantic_scope_kind::impl_scope ||
         kind == semantic_scope_kind::type_scope ||
         kind == semantic_scope_kind::concept_scope;
}

/// Builds the signature scope (type parameters and parameter bindings) and
/// body scope for a function-shaped declaration, then walks its body. Shared
/// by both `func_decl` and any future function-like construct that needs the
/// same signature/body scope split.
auto walk_function_like_body(const ast::func_decl &decl, scope_id parent_scope,
                             const scope_build_context &context,
                             semantic_scope_kind signature_kind,
                             semantic_scope_kind body_kind) -> void {
  auto signature_scope =
      add_scope(context.session, signature_kind, parent_scope, context.file_id,
                context.module_name, decl.name,
                source_location{
                    .file_id = context.file_id,
                    .span = decl.span,
                });

  for (const auto &type_param : decl.type_params) {
    if (type_param.name.empty()) {
      continue;
    }
    add_symbol(context.session, signature_scope,
               semantic_symbol_spec{
                   .name = type_param.name,
                   .kind = semantic_symbol_kind::type_parameter_symbol,
                   .name_space = symbol_namespace::type_parameter_namespace,
                   .visibility = ast::visibility::def,
                   .location =
                       source_location{
                           .file_id = context.file_id,
                           .span = type_param.span,
                       },
               });
  }

  for (const auto &param : decl.params) {
    if (param.pattern != nullptr) {
      record_node_scope(context.session, param.pattern.get(), signature_scope);
      auto bindings = std::vector<pattern_binding_spec>{};
      collect_pattern_bindings(*param.pattern, context.file_id, bindings);
      for (const auto &binding : bindings) {
        add_symbol(context.session, signature_scope,
                   semantic_symbol_spec{
                       .name = binding.name,
                       .kind = semantic_symbol_kind::parameter_symbol,
                       .name_space = symbol_namespace::value_namespace,
                       .visibility = ast::visibility::def,
                       .location = binding.location,
                   });
      }
    }
    walk_child(param.type_annotation, signature_scope, context);
    walk_child(param.default_value, signature_scope, context);
  }
  walk_type_param_bounds(decl.type_params, signature_scope, context);
  walk_child(decl.modifiers.async_context, signature_scope, context);
  walk_child(decl.return_type, signature_scope, context);
  for (const auto &constraint : decl.where_constraints) {
    walk_child(constraint.subject, signature_scope, context);
    walk_child(constraint.bound_or_type, signature_scope, context);
  }
  for (const auto &contract : decl.contracts) {
    walk_child(contract.condition, signature_scope, context);
  }

  auto body_scope = add_scope(context.session, body_kind, signature_scope,
                              context.file_id, context.module_name, decl.name,
                              source_location{
                                  .file_id = context.file_id,
                                  .span = decl.span,
                              });

  if (decl.body_expr != nullptr) {
    walk_node(*decl.body_expr, body_scope, context);
  }
  walk_node_list(decl.body_stmts, body_scope, context);
}

/// Builds the signature scope (parameter bindings) and body scope for a
/// lambda expression, then walks its body.
auto walk_lambda_body(const ast::lambda_expr &lambda, scope_id parent_scope,
                      const scope_build_context &context) -> void {
  auto signature_scope =
      add_scope(context.session, semantic_scope_kind::lambda_signature_scope,
                parent_scope, context.file_id, context.module_name, "<lambda>",
                source_location{
                    .file_id = context.file_id,
                    .span = lambda.span,
                });

  for (const auto &param : lambda.params) {
    if (param.pattern != nullptr) {
      record_node_scope(context.session, param.pattern.get(), signature_scope);
      auto bindings = std::vector<pattern_binding_spec>{};
      collect_pattern_bindings(
          *dynamic_cast<const ast::pattern *>(param.pattern.get()),
          context.file_id, bindings);
      for (const auto &binding : bindings) {
        add_symbol(context.session, signature_scope,
                   semantic_symbol_spec{
                       .name = binding.name,
                       .kind = semantic_symbol_kind::parameter_symbol,
                       .name_space = symbol_namespace::value_namespace,
                       .visibility = ast::visibility::def,
                       .location = binding.location,
                   });
      }
    }
    walk_child(param.type_annotation, signature_scope, context);
  }
  walk_child(lambda.return_type, signature_scope, context);

  auto body_scope = add_scope(
      context.session, semantic_scope_kind::lambda_body_scope, signature_scope,
      context.file_id, context.module_name, "<lambda>",
      source_location{
          .file_id = context.file_id,
          .span = lambda.span,
      });

  if (lambda.body_expr != nullptr) {
    walk_node(*lambda.body_expr, body_scope, context);
  }
  walk_node_list(lambda.body_stmts, body_scope, context);
}

/// Records `active_scope` as the scope for `node`, then builds any scopes
/// and symbols the node itself introduces (function/lambda bodies, `let`
/// bindings, `if`/`match`/`for` branch scopes, type/trait/impl/concept
/// scopes, nested modules, ...), recursing into children as needed.
///
/// Returns the scope subsequent sibling statements should use: unchanged for
/// most constructs, but extended for binding statements (`let`, `var`) so a
/// binding is visible to the statements that follow it in the same block.
auto walk_node(const ast::node &node, scope_id active_scope,
               const scope_build_context &context) -> scope_id {
  record_node_scope(context.session, &node, active_scope);

  switch (node.kind) {
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(node);
    // A top-level item (module/trait/impl/type/concept scope) is already
    // pre-registered by the binding walk's own item pass before bodies are
    // walked at all; registering it again here would duplicate the symbol.
    // Only a `def` nested inside a function/lambda body has no such pass and
    // needs registering here instead.
    if (!is_item_scope(context.session.scopes[active_scope].kind)) {
      if (auto spec = module_symbol_spec(node, context.file_id)) {
        add_symbol(context.session, active_scope, *spec);
      }
    }
    walk_function_like_body(decl, active_scope, context,
                            semantic_scope_kind::function_signature_scope,
                            semantic_scope_kind::function_body_scope);
    return active_scope;
  }

  case ast::node_kind::lambda_expr:
    walk_lambda_body(dynamic_cast<const ast::lambda_expr &>(node), active_scope,
                     context);
    return active_scope;

  case ast::node_kind::let_stmt: {
    const auto &stmt = dynamic_cast<const ast::let_stmt &>(node);
    if (stmt.pattern != nullptr) {
      walk_node(*stmt.pattern, active_scope, context);
    }
    if (stmt.type_annotation != nullptr) {
      walk_node(*stmt.type_annotation, active_scope, context);
    }
    if (stmt.initializer != nullptr) {
      walk_node(*stmt.initializer, active_scope, context);
    }
    if (!stmt.else_body.empty()) {
      const auto else_scope = create_block_scope(
          active_scope, context, semantic_scope_kind::branch_scope, "let else",
          stmt.span);
      walk_node_list(stmt.else_body, else_scope, context);
    }

    auto bindings = std::vector<pattern_binding_spec>{};
    if (stmt.pattern != nullptr) {
      collect_pattern_bindings(*stmt.pattern, context.file_id, bindings);
    }
    return extend_scope_with_bindings(
        context.session, active_scope, semantic_scope_kind::block_scope,
        "let binding", context.file_id, context.module_name, bindings,
        semantic_symbol_kind::local_binding_symbol);
  }

  case ast::node_kind::var_stmt: {
    const auto &stmt = dynamic_cast<const ast::var_stmt &>(node);
    if (stmt.type_annotation != nullptr) {
      walk_node(*stmt.type_annotation, active_scope, context);
    }
    if (stmt.initializer != nullptr) {
      walk_node(*stmt.initializer, active_scope, context);
    }
    if (stmt.name.empty()) {
      return active_scope;
    }
    return extend_scope_with_bindings(
        context.session, active_scope, semantic_scope_kind::block_scope,
        "var binding", context.file_id, context.module_name,
        {pattern_binding_spec{
            .name = stmt.name,
            .location =
                source_location{
                    .file_id = context.file_id,
                    .span = stmt.span,
                },
        }},
        semantic_symbol_kind::mutable_local_symbol);
  }

  case ast::node_kind::expr_stmt: {
    const auto &stmt = dynamic_cast<const ast::expr_stmt &>(node);
    if (stmt.expr != nullptr) {
      walk_node(*stmt.expr, active_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::return_stmt: {
    const auto &stmt = dynamic_cast<const ast::return_stmt &>(node);
    if (stmt.value != nullptr) {
      walk_node(*stmt.value, active_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::assign_stmt: {
    const auto &stmt = dynamic_cast<const ast::assign_stmt &>(node);
    if (stmt.target != nullptr) {
      walk_node(*stmt.target, active_scope, context);
    }
    if (stmt.value != nullptr) {
      walk_node(*stmt.value, active_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::if_stmt: {
    const auto &stmt = dynamic_cast<const ast::if_stmt &>(node);
    for (const auto &branch : stmt.branches) {
      if (branch.condition != nullptr) {
        walk_node(*branch.condition, active_scope, context);
      }
      if (branch.let_expr != nullptr) {
        walk_node(*branch.let_expr, active_scope, context);
      }
      auto branch_scope = create_block_scope(active_scope, context,
                                             semantic_scope_kind::branch_scope,
                                             "if branch", branch.span);
      if (branch.let_pattern != nullptr) {
        walk_node(*branch.let_pattern, branch_scope, context);
        auto bindings = std::vector<pattern_binding_spec>{};
        collect_pattern_bindings(
            *dynamic_cast<const ast::pattern *>(branch.let_pattern.get()),
            context.file_id, bindings);
        branch_scope = extend_scope_with_bindings(
            context.session, branch_scope, semantic_scope_kind::branch_scope,
            "if let bindings", context.file_id, context.module_name, bindings,
            semantic_symbol_kind::pattern_binding_symbol);
      }
      walk_node_list(branch.body, branch_scope, context);
    }
    if (!stmt.else_body.empty()) {
      const auto else_scope = create_block_scope(
          active_scope, context, semantic_scope_kind::branch_scope,
          "else branch", stmt.span);
      walk_node_list(stmt.else_body, else_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::if_expr: {
    const auto &expr = dynamic_cast<const ast::if_expr &>(node);
    for (const auto &branch : expr.branches) {
      if (branch.condition != nullptr) {
        walk_node(*branch.condition, active_scope, context);
      }
      if (branch.let_expr != nullptr) {
        walk_node(*branch.let_expr, active_scope, context);
      }
      auto branch_scope = create_block_scope(active_scope, context,
                                             semantic_scope_kind::branch_scope,
                                             "if branch", branch.span);
      if (branch.let_pattern != nullptr) {
        walk_node(*branch.let_pattern, branch_scope, context);
        auto bindings = std::vector<pattern_binding_spec>{};
        collect_pattern_bindings(
            *dynamic_cast<const ast::pattern *>(branch.let_pattern.get()),
            context.file_id, bindings);
        branch_scope = extend_scope_with_bindings(
            context.session, branch_scope, semantic_scope_kind::branch_scope,
            "if let bindings", context.file_id, context.module_name, bindings,
            semantic_symbol_kind::pattern_binding_symbol);
      }
      walk_node_list(branch.body, branch_scope, context);
    }
    if (!expr.else_body.empty()) {
      const auto else_scope = create_block_scope(
          active_scope, context, semantic_scope_kind::branch_scope,
          "else branch", expr.span);
      walk_node_list(expr.else_body, else_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::while_stmt: {
    const auto &stmt = dynamic_cast<const ast::while_stmt &>(node);
    if (stmt.condition != nullptr) {
      walk_node(*stmt.condition, active_scope, context);
    }
    if (stmt.let_expr != nullptr) {
      walk_node(*stmt.let_expr, active_scope, context);
    }
    auto loop_scope = create_block_scope(active_scope, context,
                                         semantic_scope_kind::loop_scope,
                                         "while loop", stmt.span);
    if (stmt.let_pattern != nullptr) {
      walk_node(*stmt.let_pattern, loop_scope, context);
      auto bindings = std::vector<pattern_binding_spec>{};
      collect_pattern_bindings(*stmt.let_pattern, context.file_id, bindings);
      loop_scope = extend_scope_with_bindings(
          context.session, loop_scope, semantic_scope_kind::loop_scope,
          "while let bindings", context.file_id, context.module_name, bindings,
          semantic_symbol_kind::pattern_binding_symbol);
    }
    walk_node_list(stmt.body, loop_scope, context);
    return active_scope;
  }

  case ast::node_kind::for_stmt: {
    const auto &stmt = dynamic_cast<const ast::for_stmt &>(node);
    if (stmt.iterable != nullptr) {
      walk_node(*stmt.iterable, active_scope, context);
    }
    auto loop_scope = create_block_scope(active_scope, context,
                                         semantic_scope_kind::loop_scope,
                                         "for loop", stmt.span);
    auto bindings = std::vector<pattern_binding_spec>{};
    for (const auto &pattern : stmt.patterns) {
      if (pattern != nullptr) {
        walk_node(*pattern, loop_scope, context);
        collect_pattern_bindings(*pattern, context.file_id, bindings);
      }
    }
    loop_scope = extend_scope_with_bindings(
        context.session, loop_scope, semantic_scope_kind::loop_scope,
        "for bindings", context.file_id, context.module_name, bindings,
        semantic_symbol_kind::pattern_binding_symbol);
    if (stmt.guard != nullptr) {
      walk_node(*stmt.guard, loop_scope, context);
    }
    walk_node_list(stmt.body, loop_scope, context);
    return active_scope;
  }

  case ast::node_kind::for_expr: {
    const auto &expr = dynamic_cast<const ast::for_expr &>(node);
    auto current_scope = create_block_scope(active_scope, context,
                                            semantic_scope_kind::loop_scope,
                                            "for expr", expr.span);
    for (const auto &clause : expr.clauses) {
      if (clause.iterable != nullptr) {
        walk_node(*clause.iterable, current_scope, context);
      }
      auto bindings = std::vector<pattern_binding_spec>{};
      for (const auto &pattern : clause.patterns) {
        if (pattern != nullptr) {
          walk_node(*pattern, current_scope, context);
          collect_pattern_bindings(
              *dynamic_cast<const ast::pattern *>(pattern.get()),
              context.file_id, bindings);
        }
      }
      current_scope = extend_scope_with_bindings(
          context.session, current_scope, semantic_scope_kind::loop_scope,
          "for clause bindings", context.file_id, context.module_name, bindings,
          semantic_symbol_kind::pattern_binding_symbol);
    }
    if (expr.guard != nullptr) {
      walk_node(*expr.guard, current_scope, context);
    }
    if (expr.yield_expr != nullptr) {
      walk_node(*expr.yield_expr, current_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::match_stmt: {
    const auto &stmt = dynamic_cast<const ast::match_stmt &>(node);
    if (stmt.subject != nullptr) {
      walk_node(*stmt.subject, active_scope, context);
    }
    for (const auto &arm : stmt.arms) {
      auto arm_scope = create_block_scope(active_scope, context,
                                          semantic_scope_kind::match_arm_scope,
                                          "match arm", arm.span);
      if (arm.pattern != nullptr) {
        walk_node(*arm.pattern, arm_scope, context);
        auto bindings = std::vector<pattern_binding_spec>{};
        collect_pattern_bindings(
            *dynamic_cast<const ast::pattern *>(arm.pattern.get()),
            context.file_id, bindings);
        arm_scope = extend_scope_with_bindings(
            context.session, arm_scope, semantic_scope_kind::match_arm_scope,
            "match arm bindings", context.file_id, context.module_name,
            bindings, semantic_symbol_kind::pattern_binding_symbol);
      }
      if (arm.guard != nullptr) {
        walk_node(*arm.guard, arm_scope, context);
      }
      if (arm.body_expr != nullptr) {
        walk_node(*arm.body_expr, arm_scope, context);
      }
      walk_node_list(arm.body_stmts, arm_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::match_expr: {
    const auto &expr = dynamic_cast<const ast::match_expr &>(node);
    if (expr.subject != nullptr) {
      walk_node(*expr.subject, active_scope, context);
    }
    for (const auto &arm : expr.arms) {
      auto arm_scope = create_block_scope(active_scope, context,
                                          semantic_scope_kind::match_arm_scope,
                                          "match arm", arm.span);
      if (arm.pattern != nullptr) {
        walk_node(*arm.pattern, arm_scope, context);
        auto bindings = std::vector<pattern_binding_spec>{};
        collect_pattern_bindings(
            *dynamic_cast<const ast::pattern *>(arm.pattern.get()),
            context.file_id, bindings);
        arm_scope = extend_scope_with_bindings(
            context.session, arm_scope, semantic_scope_kind::match_arm_scope,
            "match arm bindings", context.file_id, context.module_name,
            bindings, semantic_symbol_kind::pattern_binding_symbol);
      }
      if (arm.guard != nullptr) {
        walk_node(*arm.guard, arm_scope, context);
      }
      if (arm.body_expr != nullptr) {
        walk_node(*arm.body_expr, arm_scope, context);
      }
      walk_node_list(arm.body_stmts, arm_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::block_expr: {
    const auto &expr = dynamic_cast<const ast::block_expr &>(node);
    const auto block_scope = create_block_scope(
        active_scope, context, semantic_scope_kind::block_scope, "block",
        expr.span);
    walk_node_list(expr.stmts, block_scope, context);
    return active_scope;
  }

  case ast::node_kind::async_expr: {
    const auto &expr = dynamic_cast<const ast::async_expr &>(node);
    const auto body_scope = create_block_scope(active_scope, context,
                                               semantic_scope_kind::block_scope,
                                               "async", expr.span);
    walk_node_list(expr.body, body_scope, context);
    return active_scope;
  }

  case ast::node_kind::crew_expr: {
    const auto &expr = dynamic_cast<const ast::crew_expr &>(node);
    const auto body_scope =
        create_block_scope(active_scope, context,
                           semantic_scope_kind::block_scope, "crew", expr.span);
    walk_node_list(expr.body, body_scope, context);
    return active_scope;
  }

  case ast::node_kind::crew_stmt: {
    const auto &stmt = dynamic_cast<const ast::crew_stmt &>(node);
    const auto body_scope =
        create_block_scope(active_scope, context,
                           semantic_scope_kind::block_scope, "crew", stmt.span);
    walk_node_list(stmt.body, body_scope, context);
    return active_scope;
  }

  case ast::node_kind::on_expr: {
    const auto &expr = dynamic_cast<const ast::on_expr &>(node);
    if (expr.context_type != nullptr) {
      walk_node(*expr.context_type, active_scope, context);
    }
    if (expr.sender != nullptr) {
      walk_node(*expr.sender, active_scope, context);
    }
    const auto body_scope =
        create_block_scope(active_scope, context,
                           semantic_scope_kind::block_scope, "on", expr.span);
    walk_node_list(expr.body, body_scope, context);
    return active_scope;
  }

  case ast::node_kind::where_expr: {
    const auto &expr = dynamic_cast<const ast::where_expr &>(node);
    auto where_scope = create_block_scope(active_scope, context,
                                          semantic_scope_kind::where_scope,
                                          "where", expr.span);
    for (const auto &binding : expr.bindings) {
      if (binding.value != nullptr) {
        walk_node(*binding.value, where_scope, context);
      }
      if (!binding.name.empty()) {
        where_scope = extend_scope_with_bindings(
            context.session, where_scope, semantic_scope_kind::where_scope,
            "where binding", context.file_id, context.module_name,
            {pattern_binding_spec{
                .name = binding.name,
                .location =
                    source_location{
                        .file_id = context.file_id,
                        .span = binding.span,
                    },
            }},
            semantic_symbol_kind::where_binding_symbol);
      }
    }
    if (expr.inner != nullptr) {
      walk_node(*expr.inner, where_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::type_decl: {
    const auto &decl = dynamic_cast<const ast::type_decl &>(node);
    if (!decl.type_params.empty() || decl.definition != nullptr ||
        decl.invariant != nullptr) {
      auto type_scope = add_scope(
          context.session, semantic_scope_kind::type_scope, active_scope,
          context.file_id, context.module_name, decl.name,
          source_location{
              .file_id = context.file_id,
              .span = decl.span,
          });
      for (const auto &type_param : decl.type_params) {
        if (type_param.name.empty()) {
          continue;
        }
        add_symbol(context.session, type_scope,
                   semantic_symbol_spec{
                       .name = type_param.name,
                       .kind = semantic_symbol_kind::type_parameter_symbol,
                       .name_space = symbol_namespace::type_parameter_namespace,
                       .visibility = ast::visibility::def,
                       .location =
                           source_location{
                               .file_id = context.file_id,
                               .span = type_param.span,
                           },
                   });
      }
      if (decl.definition != nullptr) {
        walk_node(*decl.definition, type_scope, context);
      }
      if (decl.invariant != nullptr) {
        walk_node(*decl.invariant, type_scope, context);
      }
    }
    return active_scope;
  }

  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(node);
    auto trait_scope =
        add_scope(context.session, semantic_scope_kind::trait_scope,
                  active_scope, context.file_id, context.module_name, decl.name,
                  source_location{
                      .file_id = context.file_id,
                      .span = decl.span,
                  });
    for (const auto &type_param : decl.type_params) {
      if (type_param.name.empty()) {
        continue;
      }
      add_symbol(context.session, trait_scope,
                 semantic_symbol_spec{
                     .name = type_param.name,
                     .kind = semantic_symbol_kind::type_parameter_symbol,
                     .name_space = symbol_namespace::type_parameter_namespace,
                     .visibility = ast::visibility::def,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = type_param.span,
                         },
                 });
    }
    walk_node_list(decl.items, trait_scope, context);
    return active_scope;
  }

  case ast::node_kind::impl_decl: {
    const auto &decl = dynamic_cast<const ast::impl_decl &>(node);
    auto impl_scope =
        add_scope(context.session, semantic_scope_kind::impl_scope,
                  active_scope, context.file_id, context.module_name, "impl",
                  source_location{
                      .file_id = context.file_id,
                      .span = decl.span,
                  });
    for (const auto &type_param : decl.type_params) {
      if (type_param.name.empty()) {
        continue;
      }
      add_symbol(context.session, impl_scope,
                 semantic_symbol_spec{
                     .name = type_param.name,
                     .kind = semantic_symbol_kind::type_parameter_symbol,
                     .name_space = symbol_namespace::type_parameter_namespace,
                     .visibility = ast::visibility::def,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = type_param.span,
                         },
                 });
    }
    if (decl.trait_type != nullptr) {
      walk_node(*decl.trait_type, impl_scope, context);
    }
    if (decl.for_type != nullptr) {
      walk_node(*decl.for_type, impl_scope, context);
    }
    walk_node_list(decl.items, impl_scope, context);
    return active_scope;
  }

  case ast::node_kind::concept_decl: {
    const auto &decl = dynamic_cast<const ast::concept_decl &>(node);
    auto concept_scope =
        add_scope(context.session, semantic_scope_kind::concept_scope,
                  active_scope, context.file_id, context.module_name, decl.name,
                  source_location{
                      .file_id = context.file_id,
                      .span = decl.span,
                  });
    for (const auto &param : decl.params) {
      if (param.name.empty()) {
        continue;
      }
      add_symbol(context.session, concept_scope,
                 semantic_symbol_spec{
                     .name = param.name,
                     .kind = semantic_symbol_kind::type_parameter_symbol,
                     .name_space = symbol_namespace::type_parameter_namespace,
                     .visibility = ast::visibility::def,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = param.span,
                         },
                 });
    }
    for (const auto &constraint : decl.constraints) {
      if (constraint.subject != nullptr) {
        walk_node(*constraint.subject, concept_scope, context);
      }
      if (constraint.bound_or_expr != nullptr) {
        walk_node(*constraint.bound_or_expr, concept_scope, context);
      }
    }
    return active_scope;
  }

  case ast::node_kind::associated_type_decl_node: {
    const auto &assoc =
        dynamic_cast<const ast::associated_type_decl_node &>(node);
    if (!assoc.value.name.empty()) {
      add_symbol(context.session, active_scope,
                 semantic_symbol_spec{
                     .name = assoc.value.name,
                     .kind = semantic_symbol_kind::associated_type_symbol,
                     .name_space = symbol_namespace::associated_type_namespace,
                     .visibility = assoc.value.visibility,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = assoc.value.span,
                         },
                 });
    }
    if (assoc.value.default_type != nullptr) {
      walk_node(*assoc.value.default_type, active_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::associated_type_def_node: {
    const auto &assoc =
        dynamic_cast<const ast::associated_type_def_node &>(node);
    if (!assoc.value.name.empty()) {
      add_symbol(context.session, active_scope,
                 semantic_symbol_spec{
                     .name = assoc.value.name,
                     .kind = semantic_symbol_kind::associated_type_symbol,
                     .name_space = symbol_namespace::associated_type_namespace,
                     .visibility = ast::visibility::def,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = assoc.value.span,
                         },
                 });
    }
    if (assoc.value.type != nullptr) {
      walk_node(*assoc.value.type, active_scope, context);
    }
    return active_scope;
  }

  case ast::node_kind::sub_module_decl: {
    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(node);
    if (!decl.items.empty()) {
      auto child_context = scope_build_context{
          .session = context.session,
          .file_id = context.file_id,
          .module_name = append_module_name(context.module_name, decl.name),
      };

      const auto module_scope = add_scope(
          context.session, semantic_scope_kind::module_scope, active_scope,
          context.file_id, child_context.module_name, child_context.module_name,
          source_location{
              .file_id = context.file_id,
              .span = decl.span,
          });

      auto module_record = module_scope_record{
          .module_name = child_context.module_name,
          .file_id = context.file_id,
          .scope = module_scope,
          .symbols = {},
      };

      for (const auto &item : decl.items) {
        if (item == nullptr || item->has_error) {
          continue;
        }
        if (const auto spec = module_symbol_spec(*item, context.file_id)) {
          module_record.symbols.push_back(
              add_symbol(context.session, module_scope, *spec));
        }
      }
      context.session.module_scopes.push_back(std::move(module_record));

      for (const auto &item : decl.items) {
        if (item != nullptr) {
          walk_node(*item, module_scope, child_context);
        }
      }
    }
    return active_scope;
  }

  case ast::node_kind::extend_decl: {
    const auto &decl = dynamic_cast<const ast::extend_decl &>(node);
    auto extend_scope =
        add_scope(context.session, semantic_scope_kind::impl_scope,
                  active_scope, context.file_id, context.module_name, "extend",
                  source_location{
                      .file_id = context.file_id,
                      .span = decl.span,
                  });
    for (const auto &type_param : decl.type_params) {
      if (type_param.name.empty()) {
        continue;
      }
      add_symbol(context.session, extend_scope,
                 semantic_symbol_spec{
                     .name = type_param.name,
                     .kind = semantic_symbol_kind::type_parameter_symbol,
                     .name_space = symbol_namespace::type_parameter_namespace,
                     .visibility = ast::visibility::def,
                     .location =
                         source_location{
                             .file_id = context.file_id,
                             .span = type_param.span,
                         },
                 });
    }
    walk_type_param_bounds(decl.type_params, extend_scope, context);
    walk_child(decl.for_type, extend_scope, context);
    walk_node_list(decl.items, extend_scope, context);
    return active_scope;
  }

  case ast::node_kind::signature_decl:
    walk_node_list(dynamic_cast<const ast::signature_decl &>(node).items,
                   active_scope, context);
    return active_scope;

  case ast::node_kind::struct_type_def:
    for (const auto &field :
         dynamic_cast<const ast::struct_type_def &>(node).body.fields) {
      walk_child(field.type, active_scope, context);
    }
    return active_scope;

  case ast::node_kind::sum_type_def:
    for (const auto &variant :
         dynamic_cast<const ast::sum_type_def &>(node).body.variants) {
      walk_children(variant.payload_types, active_scope, context);
    }
    return active_scope;

  case ast::node_kind::static_decl:
    return walk_static_decl(dynamic_cast<const ast::static_decl &>(node),
                            active_scope, context);

  case ast::node_kind::splice_stmt:
    walk_child(dynamic_cast<const ast::splice_stmt &>(node).expr, active_scope,
               context);
    return active_scope;

  // Types. None binds a name; each is walked for the expressions some of
  // them carry (array lengths, refinement predicates, value arguments).
  case ast::node_kind::named_type:
    walk_type_args(dynamic_cast<const ast::named_type &>(node).type_args,
                   active_scope, context);
    return active_scope;
  case ast::node_kind::bound_type:
    walk_bound(dynamic_cast<const ast::bound_type &>(node).value, active_scope,
               context);
    return active_scope;
  case ast::node_kind::existential_type:
    walk_bound(dynamic_cast<const ast::existential_type &>(node).value,
               active_scope, context);
    return active_scope;
  case ast::node_kind::tuple_type:
    walk_children(dynamic_cast<const ast::tuple_type &>(node).elements,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::slice_type:
    walk_child(dynamic_cast<const ast::slice_type &>(node).element,
               active_scope, context);
    return active_scope;
  case ast::node_kind::array_type: {
    const auto &type = dynamic_cast<const ast::array_type &>(node);
    walk_child(type.element, active_scope, context);
    walk_child(type.size, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::ref_type:
    walk_child(dynamic_cast<const ast::ref_type &>(node).inner, active_scope,
               context);
    return active_scope;
  case ast::node_kind::ptr_type:
    walk_child(dynamic_cast<const ast::ptr_type &>(node).inner, active_scope,
               context);
    return active_scope;
  case ast::node_kind::mut_type:
    walk_child(dynamic_cast<const ast::mut_type &>(node).inner, active_scope,
               context);
    return active_scope;
  case ast::node_kind::fn_type: {
    const auto &type = dynamic_cast<const ast::fn_type &>(node);
    walk_children(type.param_types, active_scope, context);
    walk_child(type.return_type, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::splice_type:
    walk_child(dynamic_cast<const ast::splice_type &>(node).operand,
               active_scope, context);
    return active_scope;
  case ast::node_kind::union_type:
    walk_children(dynamic_cast<const ast::union_type &>(node).alternatives,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::refinement_type: {
    const auto &type = dynamic_cast<const ast::refinement_type &>(node);
    walk_child(type.base, active_scope, context);
    walk_child(type.predicate, active_scope, context);
    return active_scope;
  }

  // Expressions with no binders of their own: walked so every nested node
  // (a lambda or block inside an argument, say) gets its scope.
  case ast::node_kind::binary_expr: {
    const auto &expr = dynamic_cast<const ast::binary_expr &>(node);
    walk_child(expr.lhs, active_scope, context);
    walk_child(expr.rhs, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::unary_expr:
    walk_child(dynamic_cast<const ast::unary_expr &>(node).operand,
               active_scope, context);
    return active_scope;
  case ast::node_kind::call_expr: {
    const auto &expr = dynamic_cast<const ast::call_expr &>(node);
    walk_child(expr.callee, active_scope, context);
    for (const auto &arg : expr.args) {
      walk_child(arg.value, active_scope, context);
    }
    return active_scope;
  }
  case ast::node_kind::index_expr: {
    const auto &expr = dynamic_cast<const ast::index_expr &>(node);
    walk_child(expr.object, active_scope, context);
    walk_child(expr.index, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::field_expr: {
    const auto &expr = dynamic_cast<const ast::field_expr &>(node);
    walk_child(expr.object, active_scope, context);
    walk_children(expr.generic_args, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::cast_expr: {
    const auto &expr = dynamic_cast<const ast::cast_expr &>(node);
    walk_child(expr.operand, active_scope, context);
    walk_child(expr.target_type, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::try_expr:
    walk_child(dynamic_cast<const ast::try_expr &>(node).operand, active_scope,
               context);
    return active_scope;
  case ast::node_kind::tuple_expr:
    walk_children(dynamic_cast<const ast::tuple_expr &>(node).elements,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::array_expr: {
    const auto &expr = dynamic_cast<const ast::array_expr &>(node);
    walk_children(expr.elements, active_scope, context);
    walk_child(expr.fill_value, active_scope, context);
    walk_child(expr.fill_count, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::struct_expr: {
    const auto &expr = dynamic_cast<const ast::struct_expr &>(node);
    walk_child(expr.type_name, active_scope, context);
    walk_type_args(expr.type_args, active_scope, context);
    for (const auto &field : expr.fields) {
      walk_child(field.value, active_scope, context);
    }
    return active_scope;
  }
  case ast::node_kind::group_expr:
    walk_child(dynamic_cast<const ast::group_expr &>(node).inner, active_scope,
               context);
    return active_scope;
  case ast::node_kind::await_expr:
    walk_child(dynamic_cast<const ast::await_expr &>(node).operand,
               active_scope, context);
    return active_scope;
  case ast::node_kind::yield_expr:
    walk_child(dynamic_cast<const ast::yield_expr &>(node).value, active_scope,
               context);
    return active_scope;
  case ast::node_kind::par_expr:
    walk_children(dynamic_cast<const ast::par_expr &>(node).branches,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::race_expr:
    walk_children(dynamic_cast<const ast::race_expr &>(node).branches,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::quote_expr:
    // The fragment's bindings belong to wherever it is spliced, which this
    // walk can't know; its nodes are still given the scope it is written in.
    walk_child(dynamic_cast<const ast::quote_expr &>(node).parsed_body,
               active_scope, context);
    return active_scope;
  case ast::node_kind::splice_expr:
    walk_child(dynamic_cast<const ast::splice_expr &>(node).operand,
               active_scope, context);
    return active_scope;
  case ast::node_kind::static_expr:
    walk_child(dynamic_cast<const ast::static_expr &>(node).operand,
               active_scope, context);
    return active_scope;
  case ast::node_kind::interpolated_string_expr:
    for (const auto &segment :
         dynamic_cast<const ast::interpolated_string_expr &>(node).segments) {
      walk_child(segment.value, active_scope, context);
      if (const auto *width =
              std::get_if<ast::ptr<ast::expr>>(&segment.spec.width)) {
        walk_child(*width, active_scope, context);
      }
      if (const auto *precision =
              std::get_if<ast::ptr<ast::expr>>(&segment.spec.precision)) {
        walk_child(*precision, active_scope, context);
      }
    }
    return active_scope;

  // Patterns. Their bindings are added by the construct that owns the
  // pattern (`let`, a `match` arm, a parameter, ...); walking them only
  // reaches the expressions a range pattern carries.
  case ast::node_kind::constructor_pattern:
    walk_children(dynamic_cast<const ast::constructor_pattern &>(node).args,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::tuple_pattern:
    walk_children(dynamic_cast<const ast::tuple_pattern &>(node).elements,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::struct_pattern:
    for (const auto &field :
         dynamic_cast<const ast::struct_pattern &>(node).fields) {
      walk_child(field.pattern, active_scope, context);
    }
    return active_scope;
  case ast::node_kind::array_pattern:
    walk_children(dynamic_cast<const ast::array_pattern &>(node).elements,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::range_pattern: {
    const auto &pattern = dynamic_cast<const ast::range_pattern &>(node);
    walk_child(pattern.start, active_scope, context);
    walk_child(pattern.end, active_scope, context);
    return active_scope;
  }
  case ast::node_kind::option_pattern:
    walk_child(dynamic_cast<const ast::option_pattern &>(node).inner,
               active_scope, context);
    return active_scope;
  case ast::node_kind::result_pattern:
    walk_child(dynamic_cast<const ast::result_pattern &>(node).inner,
               active_scope, context);
    return active_scope;
  case ast::node_kind::ref_pattern:
    walk_child(dynamic_cast<const ast::ref_pattern &>(node).inner, active_scope,
               context);
    return active_scope;
  case ast::node_kind::or_pattern:
    walk_children(dynamic_cast<const ast::or_pattern &>(node).alternatives,
                  active_scope, context);
    return active_scope;
  case ast::node_kind::group_pattern:
    walk_child(dynamic_cast<const ast::group_pattern &>(node).inner,
               active_scope, context);
    return active_scope;

  // Leaves: nothing beneath them to scope. Listed rather than defaulted so a
  // new node kind is a compile error here until someone decides how it is
  // walked.
  case ast::node_kind::error_node:
  case ast::node_kind::file_node:
  case ast::node_kind::module_decl:
  case ast::node_kind::use_decl:
  case ast::node_kind::dep_decl:
  case ast::node_kind::quote_type:
  case ast::node_kind::break_stmt:
  case ast::node_kind::continue_stmt:
  case ast::node_kind::asm_stmt:
  case ast::node_kind::ident_expr:
  case ast::node_kind::literal_expr:
  case ast::node_kind::postfix_expr:
  case ast::node_kind::module_path_expr:
  case ast::node_kind::wildcard_pattern:
  case ast::node_kind::literal_pattern:
  case ast::node_kind::binding_pattern:
    return active_scope;
  }
  return active_scope;
}

/// `static let`/`static assert`/`static if`/`static for`. A `static let`
/// inside a body binds its name for the statements after it, like `let`
/// (at item scope the module-symbol pass already registered it). A `static
/// for` opens a scope holding its binders — a binder is a lexical binding
/// (spec "Dotted Names", rule 1) — for its guard, yield, and body.
auto walk_static_decl(const ast::static_decl &decl, scope_id active_scope,
                      const scope_build_context &context) -> scope_id {
  switch (decl.decl_kind) {
  case ast::static_decl_kind::binding: {
    walk_child(decl.type_annotation, active_scope, context);
    walk_child(decl.initializer, active_scope, context);
    if (is_item_scope(context.session.scopes[active_scope].kind) ||
        decl.name.empty()) {
      return active_scope;
    }
    return extend_scope_with_bindings(
        context.session, active_scope, semantic_scope_kind::block_scope,
        decl.name, context.file_id, context.module_name,
        {pattern_binding_spec{.name = decl.name,
                              .location =
                                  source_location{.file_id = context.file_id,
                                                  .span = decl.span}}},
        semantic_symbol_kind::static_binding_symbol);
  }
  case ast::static_decl_kind::assertion:
    walk_child(decl.assert_condition, active_scope, context);
    return active_scope;
  case ast::static_decl_kind::conditional_compilation: {
    walk_child(decl.if_condition, active_scope, context);
    for (const auto *body : {&decl.if_body, &decl.else_body}) {
      if (body->empty()) {
        continue;
      }
      // At item scope the branches hold items; walking them in a fresh
      // branch scope would hide nothing (items are pre-registered or not at
      // all), so only a body-level `static if` gets its own scope.
      const auto branch_scope =
          is_item_scope(context.session.scopes[active_scope].kind)
              ? active_scope
              : create_block_scope(active_scope, context,
                                   semantic_scope_kind::branch_scope,
                                   "static if", decl.span);
      walk_node_list(*body, branch_scope, context);
    }
    return active_scope;
  }
  case ast::static_decl_kind::for_inline:
  case ast::static_decl_kind::for_block: {
    walk_child(decl.for_iterable, active_scope, context);
    const auto for_scope = create_block_scope(
        active_scope, context, semantic_scope_kind::static_for_scope,
        "static for", decl.span);
    for (const auto &pattern : decl.for_patterns) {
      if (pattern == nullptr) {
        continue;
      }
      walk_node(*pattern, for_scope, context);
      auto bindings = std::vector<pattern_binding_spec>{};
      collect_pattern_bindings(*pattern, context.file_id, bindings);
      for (const auto &binding : bindings) {
        add_symbol(context.session, for_scope,
                   semantic_symbol_spec{
                       .name = binding.name,
                       .kind = semantic_symbol_kind::pattern_binding_symbol,
                       .name_space = symbol_namespace::value_namespace,
                       .visibility = ast::visibility::def,
                       .location = binding.location,
                   });
      }
    }
    walk_child(decl.for_guard, for_scope, context);
    walk_child(decl.for_yield, for_scope, context);
    walk_node_list(decl.for_body, for_scope, context);
    return active_scope;
  }
  }
  return active_scope;
}

} // namespace

/// For each input file with a valid `module` declaration: finds or creates
/// the module's scope, indexes its direct module-scope symbols, then walks
/// its items to build nested scopes/symbols and record per-node scope
/// mappings. A later file declaring a `module` path already seen earlier in
/// `inputs` extends that module's existing scope and `module_scope_record`
/// instead of creating a sibling one under the same name — Cinder's multi-file
/// module support, matching how C++ lets several translation units reopen
/// the same namespace. `module_scope_record::file_id` still names only the
/// first file, kept for diagnostics that need *a* location for the module;
/// nothing treats it as the complete file list (see `module_file_record` in
/// `module_index.h` for that).
auto build_semantic_session(const std::vector<parsed_module> &inputs)
    -> semantic_session {
  auto session = semantic_session{};
  auto module_scope_index_by_name = std::unordered_map<std::string, size_t>{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }

    const auto module_name =
        join_strings(input.ast_file->module_decl->path, ".");

    const auto existing = module_scope_index_by_name.find(module_name);
    const auto is_new_module = existing == module_scope_index_by_name.end();

    const auto module_scope =
        is_new_module ? add_scope(session, semantic_scope_kind::module_scope,
                                  k_invalid_scope_id, input.file_id,
                                  module_name, module_name,
                                  source_location{
                                      .file_id = input.file_id,
                                      .span = input.ast_file->module_decl->span,
                                  })
                      : session.module_scopes[existing->second].scope;

    if (is_new_module) {
      module_scope_index_by_name.emplace(module_name,
                                         session.module_scopes.size());
      session.module_scopes.push_back(module_scope_record{
          .module_name = module_name,
          .file_id = input.file_id,
          .scope = module_scope,
          .symbols = {},
      });
    }
    auto &module_record =
        session.module_scopes[module_scope_index_by_name.at(module_name)];

    for (const auto &item : input.ast_file->items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (const auto spec = module_symbol_spec(*item, input.file_id)) {
        module_record.symbols.push_back(
            add_symbol(session, module_scope, *spec));
      }
    }

    auto context = scope_build_context{
        .session = session,
        .file_id = input.file_id,
        .module_name = module_name,
    };
    for (const auto &item : input.ast_file->items) {
      if (item != nullptr) {
        walk_node(*item, module_scope, context);
      }
    }
  }

  return session;
}

/// Bounds-checked lookup into `session.scopes`.
auto find_semantic_scope(const semantic_session &session, scope_id id)
    -> const semantic_scope * {
  if (id == k_invalid_scope_id ||
      static_cast<size_t>(id) >= session.scopes.size()) {
    return nullptr;
  }
  return &session.scopes[id];
}

/// Bounds-checked lookup into `session.symbols`.
auto find_semantic_symbol(const semantic_session &session, symbol_id id)
    -> const semantic_symbol * {
  if (id == k_invalid_symbol_id ||
      static_cast<size_t>(id) >= session.symbols.size()) {
    return nullptr;
  }
  return &session.symbols[id];
}

/// Looks up the scope recorded for `node` while building the session.
auto find_node_scope(const semantic_session &session, const ast::node &node)
    -> std::optional<scope_id> {
  if (const auto it = session.node_scopes.find(&node);
      it != session.node_scopes.end()) {
    return it->second;
  }
  return std::nullopt;
}

/// Walks from `start_scope` outward through parent scopes, checking each
/// scope's symbols in reverse-declaration order (so a later shadowing `let`
/// wins), until `name` is found in `name_space` or scopes are exhausted.
auto resolve_symbol(const semantic_session &session, scope_id start_scope,
                    symbol_namespace name_space, std::string_view name)
    -> const semantic_symbol * {
  auto current = start_scope;
  while (current != k_invalid_scope_id) {
    const auto *scope = find_semantic_scope(session, current);
    if (scope == nullptr) {
      break;
    }
    for (unsigned int it : std::views::reverse(scope->symbols)) {
      const auto *symbol = find_semantic_symbol(session, it);
      if (symbol == nullptr) {
        continue;
      }
      if (symbol->name_space == name_space && symbol->name == name) {
        return symbol;
      }
    }
    current = scope->parent;
  }
  return nullptr;
}

} // namespace cinder::semantic
