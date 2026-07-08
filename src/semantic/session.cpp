#include "session.h"

#include <format>
#include <ranges>
#include <utility>

#include "src/semantic/binding_walk.h"
#include "src/semantic/module_index.h"

namespace kira::semantic {
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
/// bind them, via the shared `kira::semantic::collect_pattern_bindings`
/// (`binding_walk.h`) — appending each as a `pattern_binding_spec` tagged
/// with `file_id` so callers here don't need to carry it separately.
auto collect_pattern_bindings(const ast::pattern &pattern, file_id_type file_id,
                              std::vector<pattern_binding_spec> &out) -> void {
  for (const auto &binding : kira::semantic::collect_pattern_bindings(pattern)) {
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
    if (param.type_annotation != nullptr) {
      record_node_scope(context.session, param.type_annotation.get(),
                        signature_scope);
    }
    if (param.default_value != nullptr) {
      record_node_scope(context.session, param.default_value.get(),
                        signature_scope);
    }
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
    if (param.type_annotation != nullptr) {
      record_node_scope(context.session, param.type_annotation.get(),
                        signature_scope);
    }
  }

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

  default:
    return active_scope;
  }
}

} // namespace

/// For each input file with a valid `module` declaration: creates the
/// module's scope, indexes its direct module-scope symbols, then walks its
/// items to build nested scopes/symbols and record per-node scope mappings.
auto build_semantic_session(const std::vector<parsed_module> &inputs)
    -> semantic_session {
  auto session = semantic_session{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }

    const auto module_name =
        join_strings(input.ast_file->module_decl->path, ".");
    const auto module_scope =
        add_scope(session, semantic_scope_kind::module_scope,
                  k_invalid_scope_id, input.file_id, module_name, module_name,
                  source_location{
                      .file_id = input.file_id,
                      .span = input.ast_file->module_decl->span,
                  });

    auto module_record = module_scope_record{
        .module_name = module_name,
        .file_id = input.file_id,
        .scope = module_scope,
        .symbols = {},
    };

    for (const auto &item : input.ast_file->items) {
      if (item == nullptr || item->has_error) {
        continue;
      }
      if (const auto spec = module_symbol_spec(*item, input.file_id)) {
        module_record.symbols.push_back(
            add_symbol(session, module_scope, *spec));
      }
    }
    session.module_scopes.push_back(std::move(module_record));

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

} // namespace kira::semantic
