#include "src/semantic/try_from_proof.h"

#include "src/parser/ast_traversal.h"
#include <utility>

namespace cinder::semantic::detail {

/// The constant each of a refinement's value parameters was instantiated
/// with at this use. `nullopt` when one of them is still open (an `index[n]`
/// inside a function generic over `n`) — which means no runtime check can be
/// generated *here*, because the value it would compare against does not
/// exist until the enclosing template is monomorphized. The instances
/// `instantiate_generic_function` makes of it are where the check does get
/// built, with `n` a real number.
auto refinement_constants(const type_table &types, const type_entry &entry)
    -> std::optional<std::unordered_map<std::string, int64_t>> {
  auto constants = std::unordered_map<std::string, int64_t>{};
  if (entry.decl == nullptr) {
    return constants;
  }
  for (size_t i = 0; i < entry.decl->type_params.size(); ++i) {
    const auto &param = entry.decl->type_params[i];
    if (!param.is_value_param || i >= entry.args.size()) {
      continue;
    }
    const auto &slot = types.entry(entry.args[i]);
    if (slot.kind != type_kind::const_value_kind) {
      return std::nullopt;
    }
    constants.emplace(param.name, slot.value.constant);
  }
  return constants;
}

/// Rewrites a cloned predicate so it speaks about *this* proof: `self`
/// becomes the local the value was bound to, and each of the refinement's
/// value parameters becomes the constant this instantiation supplied.
///
/// Both substitutions are needed, and for the same reason. The predicate of
/// `index[n]` is `self < n`; the check generated for `index[8].try_from(x)`
/// has to be `#proof0 < 8`. Neither `self` nor `n` exists at runtime — one
/// is the abstract value, the other a compile-time parameter — so both must
/// be gone before the predicate can be compiled as ordinary code.
///
/// Takes the owning pointer, not a reference, because replacing `n` with
/// `8` swaps one node for another rather than editing it in place.
auto specialize_predicate(
    ast::ptr<ast::expr> &slot, const std::string &value_binding,
    const std::unordered_map<std::string, int64_t> &parameters) -> void {
  if (slot == nullptr) {
    return;
  }
  switch (slot->kind) {
  case ast::node_kind::ident_expr: {
    auto &ident = ast::cast<ast::ident_expr>(*slot);
    if (ident.name == "self") {
      ident.name = value_binding;
      return;
    }
    if (const auto it = parameters.find(ident.name); it != parameters.end()) {
      auto literal = ast::make<ast::literal_expr>();
      literal->span = ident.span;
      literal->lit_kind = token_kind::int_lit;
      literal->value = std::to_string(it->second);
      slot = std::move(literal);
    }
    return;
  }
  case ast::node_kind::binary_expr: {
    auto &binary = ast::cast<ast::binary_expr>(*slot);
    specialize_predicate(binary.lhs, value_binding, parameters);
    specialize_predicate(binary.rhs, value_binding, parameters);
    return;
  }
  case ast::node_kind::unary_expr: {
    auto &unary = ast::cast<ast::unary_expr>(*slot);
    specialize_predicate(unary.operand, value_binding, parameters);
    return;
  }
  case ast::node_kind::field_expr: {
    auto &field = ast::cast<ast::field_expr>(*slot);
    specialize_predicate(field.object, value_binding, parameters);
    return;
  }
  case ast::node_kind::module_path_expr: {
    // `self.value` is a path, not a field access — see `atom_key`.
    auto &path = ast::cast<ast::module_path_expr>(*slot);
    if (!path.segments.empty() && path.segments.front() == "self") {
      path.segments.front() = value_binding;
    }
    return;
  }
  case ast::node_kind::index_expr: {
    auto &index = ast::cast<ast::index_expr>(*slot);
    specialize_predicate(index.object, value_binding, parameters);
    specialize_predicate(index.index, value_binding, parameters);
    return;
  }
  case ast::node_kind::call_expr: {
    auto &call = ast::cast<ast::call_expr>(*slot);
    specialize_predicate(call.callee, value_binding, parameters);
    for (auto &arg : call.args) {
      specialize_predicate(arg.value, value_binding, parameters);
    }
    return;
  }
  default:
    return;
  }
}

/// A synthesized identifier.
///
/// The span is not decoration. `is_variant_ident` tells `@some` from `some`
/// by the span being one byte *wider* than the name — the `@` — since the
/// parser lowers both to a plain `ident_expr`. A synthesized name therefore
/// has to carry a span of exactly its own width, or the checker reads it as
/// a variant and hunts for a sum type that declares it. `is_variant` sizes
/// the span accordingly, which is how `@some` and `#proof0` are built by the
/// same function without one being mistaken for the other.
auto make_ident(std::string_view name, source_span span, bool is_variant)
    -> ast::ptr<ast::ident_expr> {
  auto ident = ast::make<ast::ident_expr>();
  ident->name = std::string(name);
  ident->span = source_span{
      .start = span.start,
      .end = span.start + static_cast<uint32_t>(name.size()) +
             (is_variant ? 1U : 0U),
  };
  return ident;
}

/// `@some(<binding>)`, or a bare `@none` when `binding` is empty.
auto make_variant_call(std::string_view variant, std::string_view binding,
                       source_span span) -> ast::ptr<ast::expr> {
  auto name = make_ident(variant, span, /*is_variant=*/true);
  if (binding.empty()) {
    return name;
  }

  auto argument = make_ident(binding, span, /*is_variant=*/false);
  const auto argument_span = argument->span;

  auto call = ast::make<ast::call_expr>();
  call->span = span;
  call->callee = std::move(name);
  call->args.push_back(ast::call_arg{.span = argument_span,
                                     .name = std::nullopt,
                                     .value = std::move(argument)});
  return call;
}

/// Wraps an expression as a statement, the shape every block body wants.
auto wrap_in_stmt(ast::ptr<ast::expr> value) -> ast::ptr<ast::node> {
  auto stmt = ast::make<ast::expr_stmt>();
  stmt->span = value->span;
  stmt->expr = std::move(value);
  return stmt;
}

/// Builds the rewritten body: a `let` binding the value's placeholder, then
/// `if <predicate>: @some(<tmp>) else: @none`.
auto build_proof_syntax(ast::ptr<ast::expr> predicate,
                        const std::string &temp_name, source_span call_span,
                        source_span value_span) -> proof_syntax {
  auto syntax = proof_syntax{};

  auto binding = ast::make<ast::binding_pattern>();
  binding->span = call_span;
  binding->name = temp_name;

  auto placeholder = make_ident(temp_name, value_span, /*is_variant=*/false);
  syntax.placeholder = placeholder.get();

  auto bind = ast::make<ast::let_stmt>();
  bind->span = call_span;
  bind->pattern = std::move(binding);
  bind->initializer = std::move(placeholder);
  syntax.bound_pattern = bind->pattern.get();

  auto some_call = make_variant_call("some", temp_name, call_span);
  auto none_value = make_variant_call("none", {}, call_span);
  syntax.some_expr = some_call.get();
  syntax.none_expr = none_value.get();
  syntax.predicate = predicate.get();

  auto branch = ast::if_branch{};
  branch.span = call_span;
  branch.condition = std::move(predicate);
  branch.body.push_back(wrap_in_stmt(std::move(some_call)));

  auto conditional = ast::make<ast::if_expr>();
  conditional->span = call_span;
  conditional->branches.push_back(std::move(branch));
  conditional->else_body.push_back(wrap_in_stmt(std::move(none_value)));
  syntax.conditional = conditional.get();

  auto block = ast::make<ast::block_expr>();
  block->span = call_span;
  block->stmts.push_back(std::move(bind));
  block->stmts.push_back(wrap_in_stmt(std::move(conditional)));
  syntax.block = std::move(block);
  return syntax;
}

} // namespace cinder::semantic::detail
