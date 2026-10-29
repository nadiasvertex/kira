#include "src/comptime/fragments.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "src/comptime/hygiene.h"
#include "src/parser/ast_traversal.h"
#include "src/parser/text_escape.h"

namespace cinder::comptime {

namespace {

[[nodiscard]] auto fail(source_span span, std::string message)
    -> std::unexpected<fragment_error> {
  return std::unexpected(
      fragment_error{.span = span, .message = std::move(message)});
}

/// Downcasts a freshly materialized node to the owned type its caller knows
/// it is — the node's kind chose the branch that built it.
template <typename T> auto as_owned(ast::ptr<ast::node> node) -> ast::ptr<T> {
  return ast::ptr<T>(static_cast<T *>(node.release()));
}

/// Whether `node`, among the shapes `materialize` rebuilds, holds a splice
/// that evaluating the quote resolves. A shape not looked into counts as
/// splice-free: its splices are resolved where the fragment is spliced.
[[nodiscard]] auto has_eager_splice(const ast::node &node) -> bool {
  switch (node.kind) {
  case ast::node_kind::splice_expr:
  case ast::node_kind::splice_type:
  case ast::node_kind::splice_stmt:
    return true;
  case ast::node_kind::binary_expr: {
    const auto &bin = ast::cast<const ast::binary_expr>(node);
    return (bin.lhs != nullptr && has_eager_splice(*bin.lhs)) ||
           (bin.rhs != nullptr && has_eager_splice(*bin.rhs));
  }
  case ast::node_kind::field_expr: {
    const auto &fld = ast::cast<const ast::field_expr>(node);
    return fld.object != nullptr && has_eager_splice(*fld.object);
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = ast::cast<const ast::return_stmt>(node);
    return ret.value != nullptr && has_eager_splice(*ret.value);
  }
  case ast::node_kind::func_decl: {
    const auto &fn = ast::cast<const ast::func_decl>(node);
    if (fn.return_type != nullptr && has_eager_splice(*fn.return_type)) {
      return true;
    }
    return std::ranges::any_of(fn.body_stmts, [](const auto &stmt) -> bool {
      return stmt != nullptr && has_eager_splice(*stmt);
    });
  }
  case ast::node_kind::impl_decl: {
    const auto &impl = ast::cast<const ast::impl_decl>(node);
    if (impl.for_type != nullptr && has_eager_splice(*impl.for_type)) {
      return true;
    }
    return std::ranges::any_of(impl.items, [](const auto &item) -> bool {
      return item != nullptr && has_eager_splice(*item);
    });
  }
  default:
    return false;
  }
}

/// Appends the splices `materialize` resolves in `node`, in the order it
/// reaches them.
auto collect_splices(const ast::node &node, std::vector<const ast::node *> &out)
    -> void {
  switch (node.kind) {
  case ast::node_kind::splice_expr:
  case ast::node_kind::splice_type:
    out.push_back(&node);
    return;
  case ast::node_kind::binary_expr: {
    const auto &bin = ast::cast<const ast::binary_expr>(node);
    if (bin.lhs != nullptr && bin.rhs != nullptr) {
      collect_splices(*bin.lhs, out);
      collect_splices(*bin.rhs, out);
    }
    return;
  }
  case ast::node_kind::ref_type: {
    const auto &ref = ast::cast<const ast::ref_type>(node);
    if (ref.inner != nullptr) {
      collect_splices(*ref.inner, out);
    }
    return;
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = ast::cast<const ast::return_stmt>(node);
    if (ret.value != nullptr) {
      collect_splices(*ret.value, out);
    }
    return;
  }
  case ast::node_kind::func_decl: {
    const auto &fn = ast::cast<const ast::func_decl>(node);
    for (const auto &param : fn.params) {
      if (param.type_annotation != nullptr) {
        collect_splices(*param.type_annotation, out);
      }
    }
    if (fn.return_type != nullptr) {
      collect_splices(*fn.return_type, out);
    }
    for (const auto &stmt : fn.body_stmts) {
      if (stmt != nullptr) {
        collect_splices(*stmt, out);
      }
    }
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &impl = ast::cast<const ast::impl_decl>(node);
    if (impl.trait_type != nullptr) {
      collect_splices(*impl.trait_type, out);
    }
    if (impl.for_type != nullptr) {
      collect_splices(*impl.for_type, out);
    }
    for (const auto &item : impl.items) {
      if (item != nullptr) {
        collect_splices(*item, out);
      }
    }
    return;
  }
  default:
    return;
  }
}

constexpr auto k_builders = std::array<std::string_view, 10>{
    "lit",    "ident", "field",        "interp_concat", "debug",
    "binary", "call",  "ctor_pattern", "match_on",      "arm"};

} // namespace

auto fragment_builder::eager_splices(const ast::quote_expr &quote)
    -> std::vector<const ast::node *> {
  auto splices = std::vector<const ast::node *>{};
  if (quote.parsed_body != nullptr && has_eager_splice(*quote.parsed_body)) {
    collect_splices(*quote.parsed_body, splices);
  }
  return splices;
}

auto fragment_builder::is_builder(std::string_view name) -> bool {
  return std::ranges::contains(k_builders, name);
}

auto fragment_builder::take_fragments() -> ast::ptr_vec<ast::node> {
  return std::move(fragments_);
}

auto fragment_builder::keep(ast::ptr<ast::node> node, value_kind kind)
    -> value {
  const auto *raw = node.get();
  fragments_.push_back(std::move(node));
  switch (kind) {
  case value_kind::stmt_fragment:
    return value::make_stmt_fragment(raw);
  case value_kind::def_expr_fragment:
    return value::make_def_expr_fragment(raw);
  case value_kind::type_expr_fragment:
    return value::make_type_expr_fragment(raw);
  case value_kind::pattern_fragment:
    return value::make_pattern_fragment(raw);
  default:
    return value::make_expr_fragment(raw);
  }
}

auto fragment_builder::quote(const ast::quote_expr &quote,
                             std::span<const value> splices)
    -> std::expected<value, fragment_error> {
  if (quote.parsed_body == nullptr) {
    return fail(quote.span, "this quoted fragment could not be parsed");
  }
  // Hygiene: a quoted `expr`/`stmt`'s own bindings get fresh names before it
  // is ever spliced, so a spliced `let temp = 99` cannot clobber a `temp` at
  // the splice site. A `def_expr`'s methods get their own scope regardless.
  // `parsed_body` is owned through a `unique_ptr`, whose pointee is mutable
  // even through a `const` quote.
  if ((quote.fragment_kind == ast::quote_fragment_kind::expr ||
       quote.fragment_kind == ast::quote_fragment_kind::stmt) &&
      hygiene_renamed_.insert(quote.parsed_body.get()).second) {
    rename_internal_bindings(*quote.parsed_body, hygiene_next_id_);
  }
  // Splices resolved now, while the values they read exist — not left as
  // syntax for a later splice site, where a `static def`'s locals are gone.
  // A quote with none keeps its body as it is.
  const ast::node *body = quote.parsed_body.get();
  auto materialized = ast::ptr<ast::node>{};
  if (!eager_splices(quote).empty()) {
    auto next = size_t{0};
    auto built = materialize(*body, splices, next);
    if (!built.has_value()) {
      return std::unexpected(built.error());
    }
    materialized = std::move(*built);
    body = materialized.get();
    fragments_.push_back(std::move(materialized));
  }
  switch (quote.fragment_kind) {
  case ast::quote_fragment_kind::expr:
    return value::make_expr_fragment(body);
  case ast::quote_fragment_kind::stmt:
    return value::make_stmt_fragment(body);
  case ast::quote_fragment_kind::def_expr:
    return value::make_def_expr_fragment(body);
  case ast::quote_fragment_kind::type_expr:
    return value::make_type_expr_fragment(body);
  case ast::quote_fragment_kind::none:
    break;
  }
  return fail(quote.span, "this quoted fragment could not be classified as an "
                          "expression, statement, or definition");
}

auto fragment_builder::materialize(const ast::node &node,
                                   std::span<const value> splices, size_t &next)
    -> std::expected<ast::ptr<ast::node>, fragment_error> {
  const auto take = [&]() -> const value * {
    return next < splices.size() ? &splices[next++] : nullptr;
  };
  const auto too_complex = [&]() -> std::unexpected<fragment_error> {
    return fail(node.span, "this quoted fragment's syntax is too complex for "
                           "a nested `~` splice inside it to be resolved "
                           "eagerly");
  };
  switch (node.kind) {
  case ast::node_kind::ident_expr:
  case ast::node_kind::module_path_expr:
  case ast::node_kind::literal_expr:
  case ast::node_kind::field_expr:
  case ast::node_kind::interpolated_string_expr: {
    auto cloned = clone_expr(node);
    if (cloned == nullptr) {
      return too_complex();
    }
    return cloned;
  }
  case ast::node_kind::splice_expr: {
    const auto *resolved = take();
    if (resolved == nullptr) {
      return fail(node.span, "this `~` splice has no value");
    }
    if (resolved->kind != value_kind::expr_fragment ||
        resolved->fragment == nullptr) {
      return fail(node.span, "this `~` splice inside a quoted fragment must "
                             "resolve to a quoted expression");
    }
    auto cloned = clone_expr(*resolved->fragment);
    if (cloned == nullptr) {
      return too_complex();
    }
    return cloned;
  }
  case ast::node_kind::splice_type: {
    const auto *resolved = take();
    if (resolved == nullptr) {
      return fail(node.span, "this `~` splice has no value");
    }
    // A bound type parameter (`T`) or a fragment holding one identifier
    // (`~(expr.ident(T.name()))`) names a type by spelling here.
    auto type_name = std::string{};
    if (resolved->kind == value_kind::type_value) {
      type_name = resolved->type_name;
    } else if (resolved->kind == value_kind::expr_fragment &&
               resolved->fragment != nullptr &&
               resolved->fragment->kind == ast::node_kind::ident_expr) {
      type_name = ast::cast<const ast::ident_expr>(*resolved->fragment).name;
    } else {
      return fail(node.span, "this `~` splice inside a quoted fragment's type "
                             "position must resolve to a type name");
    }
    auto named = ast::make<ast::named_type>();
    named->span = node.span;
    named->path.push_back(std::move(type_name));
    return named;
  }
  case ast::node_kind::binary_expr: {
    const auto &bin = ast::cast<const ast::binary_expr>(node);
    if (bin.lhs == nullptr || bin.rhs == nullptr) {
      return too_complex();
    }
    auto lhs = materialize(*bin.lhs, splices, next);
    if (!lhs.has_value()) {
      return lhs;
    }
    auto rhs = materialize(*bin.rhs, splices, next);
    if (!rhs.has_value()) {
      return rhs;
    }
    auto cloned = ast::make<ast::binary_expr>();
    cloned->span = bin.span;
    cloned->op = bin.op;
    cloned->lhs = as_owned<ast::expr>(std::move(*lhs));
    cloned->rhs = as_owned<ast::expr>(std::move(*rhs));
    return cloned;
  }
  case ast::node_kind::named_type: {
    const auto &named = ast::cast<const ast::named_type>(node);
    auto cloned = ast::make<ast::named_type>();
    cloned->span = named.span;
    cloned->path = named.path;
    // `type_args` are not cloned: no generated `impl` names a generic type.
    return cloned;
  }
  case ast::node_kind::ref_type: {
    const auto &ref = ast::cast<const ast::ref_type>(node);
    if (ref.inner == nullptr) {
      return too_complex();
    }
    auto inner = materialize(*ref.inner, splices, next);
    if (!inner.has_value()) {
      return inner;
    }
    auto cloned = ast::make<ast::ref_type>();
    cloned->span = ref.span;
    cloned->is_mut = ref.is_mut;
    cloned->inner = as_owned<ast::type_expr>(std::move(*inner));
    return cloned;
  }
  case ast::node_kind::return_stmt: {
    const auto &ret = ast::cast<const ast::return_stmt>(node);
    auto cloned = ast::make<ast::return_stmt>();
    cloned->span = ret.span;
    if (ret.value != nullptr) {
      auto value_clone = materialize(*ret.value, splices, next);
      if (!value_clone.has_value()) {
        return value_clone;
      }
      cloned->value = as_owned<ast::expr>(std::move(*value_clone));
    }
    return cloned;
  }
  case ast::node_kind::func_decl: {
    const auto &fn = ast::cast<const ast::func_decl>(node);
    auto cloned = ast::make<ast::func_decl>();
    cloned->span = fn.span;
    cloned->visibility = fn.visibility;
    cloned->modifiers.is_pure = fn.modifiers.is_pure;
    cloned->modifiers.is_async = fn.modifiers.is_async;
    cloned->modifiers.is_machine = fn.modifiers.is_machine;
    cloned->modifiers.is_static = fn.modifiers.is_static;
    cloned->modifiers.is_intrinsic = fn.modifiers.is_intrinsic;
    // Type parameters, `where` clauses and contracts are not cloned: no
    // generated method has them.
    cloned->name = fn.name;
    for (const auto &original : fn.params) {
      auto param = ast::param{};
      param.span = original.span;
      if (original.pattern != nullptr &&
          original.pattern->kind == ast::node_kind::binding_pattern) {
        const auto &binding =
            ast::cast<const ast::binding_pattern>(*original.pattern);
        auto pattern = ast::make<ast::binding_pattern>();
        pattern->span = binding.span;
        pattern->name = binding.name;
        pattern->is_mut = binding.is_mut;
        param.pattern = std::move(pattern);
      }
      if (original.type_annotation != nullptr) {
        auto type = materialize(*original.type_annotation, splices, next);
        if (!type.has_value()) {
          return type;
        }
        param.type_annotation = as_owned<ast::type_expr>(std::move(*type));
      }
      // No default value: a generated parameter (`self`, `other: &self`)
      // has none.
      cloned->params.push_back(std::move(param));
    }
    if (fn.return_type != nullptr) {
      auto type = materialize(*fn.return_type, splices, next);
      if (!type.has_value()) {
        return type;
      }
      cloned->return_type = as_owned<ast::type_expr>(std::move(*type));
    }
    for (const auto &stmt : fn.body_stmts) {
      if (stmt == nullptr) {
        continue;
      }
      auto stmt_clone = materialize(*stmt, splices, next);
      if (!stmt_clone.has_value()) {
        return stmt_clone;
      }
      cloned->body_stmts.push_back(std::move(*stmt_clone));
    }
    return cloned;
  }
  case ast::node_kind::impl_decl: {
    const auto &impl = ast::cast<const ast::impl_decl>(node);
    auto cloned = ast::make<ast::impl_decl>();
    cloned->span = impl.span;
    if (impl.trait_type != nullptr) {
      auto trait = materialize(*impl.trait_type, splices, next);
      if (!trait.has_value()) {
        return trait;
      }
      cloned->trait_type = as_owned<ast::type_expr>(std::move(*trait));
    }
    if (impl.for_type != nullptr) {
      auto target = materialize(*impl.for_type, splices, next);
      if (!target.has_value()) {
        return target;
      }
      cloned->for_type = as_owned<ast::type_expr>(std::move(*target));
    }
    // Type parameters and `where` clauses are not cloned: no generated
    // `impl` has them.
    for (const auto &item : impl.items) {
      if (item == nullptr) {
        continue;
      }
      auto item_clone = materialize(*item, splices, next);
      if (!item_clone.has_value()) {
        return item_clone;
      }
      cloned->items.push_back(std::move(*item_clone));
    }
    return cloned;
  }
  default:
    return too_complex();
  }
}

auto fragment_builder::clone_pattern(const ast::node &node)
    -> ast::ptr<ast::pattern> {
  switch (node.kind) {
  case ast::node_kind::wildcard_pattern: {
    auto cloned = ast::make<ast::wildcard_pattern>();
    cloned->span = node.span;
    return cloned;
  }
  case ast::node_kind::binding_pattern: {
    const auto &binding = ast::cast<const ast::binding_pattern>(node);
    auto cloned = ast::make<ast::binding_pattern>();
    cloned->span = binding.span;
    cloned->name = binding.name;
    cloned->is_mut = binding.is_mut;
    return cloned;
  }
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = ast::cast<const ast::constructor_pattern>(node);
    auto cloned = ast::make<ast::constructor_pattern>();
    cloned->span = ctor.span;
    cloned->name = ctor.name;
    for (const auto &arg : ctor.args) {
      if (arg == nullptr) {
        return nullptr;
      }
      auto cloned_arg = clone_pattern(*arg);
      if (cloned_arg == nullptr) {
        return nullptr;
      }
      cloned->args.push_back(std::move(cloned_arg));
    }
    return cloned;
  }
  default:
    return nullptr;
  }
}

auto fragment_builder::clone_expr(const ast::node &node)
    -> ast::ptr<ast::expr> {
  switch (node.kind) {
  case ast::node_kind::match_expr: {
    const auto &match = ast::cast<const ast::match_expr>(node);
    if (match.subject == nullptr) {
      return nullptr;
    }
    auto subject = clone_expr(*match.subject);
    if (subject == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::match_expr>();
    cloned->span = match.span;
    cloned->subject = std::move(subject);
    for (const auto &arm : match.arms) {
      // Only the arms `expr.arm` builds: no guard, an expression body.
      if (arm.pattern == nullptr || arm.guard != nullptr ||
          arm.body_expr == nullptr || !arm.body_stmts.empty()) {
        return nullptr;
      }
      auto pattern = clone_pattern(*arm.pattern);
      auto body = clone_expr(*arm.body_expr);
      if (pattern == nullptr || body == nullptr) {
        return nullptr;
      }
      ast::match_arm cloned_arm;
      cloned_arm.span = arm.span;
      cloned_arm.pattern = std::move(pattern);
      cloned_arm.body_expr = std::move(body);
      cloned->arms.push_back(std::move(cloned_arm));
    }
    return cloned;
  }
  case ast::node_kind::binary_expr: {
    const auto &bin = ast::cast<const ast::binary_expr>(node);
    if (bin.lhs == nullptr || bin.rhs == nullptr) {
      return nullptr;
    }
    auto lhs = clone_expr(*bin.lhs);
    auto rhs = clone_expr(*bin.rhs);
    if (lhs == nullptr || rhs == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::binary_expr>();
    cloned->span = bin.span;
    cloned->op = bin.op;
    cloned->lhs = std::move(lhs);
    cloned->rhs = std::move(rhs);
    return cloned;
  }
  case ast::node_kind::ident_expr: {
    const auto &ident = ast::cast<const ast::ident_expr>(node);
    auto cloned = ast::make<ast::ident_expr>();
    cloned->span = ident.span;
    cloned->name = ident.name;
    return cloned;
  }
  case ast::node_kind::module_path_expr: {
    // A dotted path (`self.value`), left unresolved by the parser.
    const auto &path = ast::cast<const ast::module_path_expr>(node);
    auto cloned = ast::make<ast::module_path_expr>();
    cloned->span = path.span;
    cloned->segments = path.segments;
    return cloned;
  }
  case ast::node_kind::literal_expr: {
    const auto &lit = ast::cast<const ast::literal_expr>(node);
    auto cloned = ast::make<ast::literal_expr>();
    cloned->span = lit.span;
    cloned->lit_kind = lit.lit_kind;
    cloned->value = lit.value;
    return cloned;
  }
  case ast::node_kind::field_expr: {
    const auto &fld = ast::cast<const ast::field_expr>(node);
    if (fld.object == nullptr) {
      return nullptr;
    }
    auto object = clone_expr(*fld.object);
    if (object == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::field_expr>();
    cloned->span = fld.span;
    cloned->object = std::move(object);
    cloned->field_name = fld.field_name;
    return cloned;
  }
  case ast::node_kind::call_expr: {
    const auto &call = ast::cast<const ast::call_expr>(node);
    if (call.callee == nullptr) {
      return nullptr;
    }
    auto callee = clone_expr(*call.callee);
    if (callee == nullptr) {
      return nullptr;
    }
    auto cloned = ast::make<ast::call_expr>();
    cloned->span = call.span;
    cloned->callee = std::move(callee);
    for (const auto &arg : call.args) {
      if (arg.value == nullptr) {
        return nullptr;
      }
      auto arg_value = clone_expr(*arg.value);
      if (arg_value == nullptr) {
        return nullptr;
      }
      cloned->args.push_back(ast::call_arg{
          .span = arg.span, .name = arg.name, .value = std::move(arg_value)});
    }
    return cloned;
  }
  case ast::node_kind::interpolated_string_expr: {
    const auto &interp = ast::cast<const ast::interpolated_string_expr>(node);
    auto cloned = ast::make<ast::interpolated_string_expr>();
    cloned->span = interp.span;
    for (const auto &segment : interp.segments) {
      auto cloned_segment = clone_segment(segment);
      if (!cloned_segment.has_value()) {
        return nullptr;
      }
      cloned->segments.push_back(std::move(*cloned_segment));
    }
    return cloned;
  }
  default:
    return nullptr;
  }
}

auto fragment_builder::append_segments(const ast::node &fragment,
                                       source_span span,
                                       std::vector<ast::interp_segment> &out)
    -> std::expected<void, fragment_error> {
  if (fragment.kind == ast::node_kind::literal_expr) {
    const auto &lit = ast::cast<const ast::literal_expr>(fragment);
    if (lit.lit_kind == token_kind::string_lit) {
      auto decoded = decode_string_literal(lit.value);
      if (!decoded.has_value()) {
        return fail(span, "`expr.interp_concat` was given a string literal "
                          "with an invalid escape sequence");
      }
      out.push_back(ast::interp_segment{.is_literal = true,
                                        .literal_text = std::move(*decoded)});
      return {};
    }
  }
  if (fragment.kind == ast::node_kind::interpolated_string_expr) {
    const auto &interp =
        ast::cast<const ast::interpolated_string_expr>(fragment);
    for (const auto &segment : interp.segments) {
      auto cloned = clone_segment(segment);
      if (!cloned.has_value()) {
        return fail(span, "`expr.interp_concat` could not clone a nested "
                          "interpolation value");
      }
      out.push_back(std::move(*cloned));
    }
    return {};
  }
  auto cloned = clone_expr(fragment);
  if (cloned == nullptr) {
    return fail(span, "`expr.interp_concat`'s argument's syntax is too "
                      "complex to embed (only identifiers, field access, "
                      "literals, and other interpolated strings are "
                      "supported)");
  }
  out.push_back(
      ast::interp_segment{.is_literal = false, .value = std::move(cloned)});
  return {};
}

auto fragment_builder::clone_segment(const ast::interp_segment &segment)
    -> std::optional<ast::interp_segment> {
  auto cloned = ast::interp_segment{};
  cloned.is_literal = segment.is_literal;
  cloned.literal_text = segment.literal_text;
  if (!segment.is_literal) {
    if (segment.value == nullptr) {
      return std::nullopt;
    }
    cloned.value = clone_expr(*segment.value);
    if (cloned.value == nullptr) {
      return std::nullopt;
    }
  }
  cloned.self_doc = segment.self_doc;
  cloned.source_text = segment.source_text;
  cloned.has_spec = segment.has_spec;
  if (segment.has_spec) {
    cloned.spec.fill = segment.spec.fill;
    cloned.spec.align = segment.spec.align;
    cloned.spec.sign = segment.spec.sign;
    cloned.spec.alternate = segment.spec.alternate;
    cloned.spec.zero_pad = segment.spec.zero_pad;
    cloned.spec.has_explicit_align = segment.spec.has_explicit_align;
    cloned.spec.type_char = segment.spec.type_char;
    cloned.spec.span = segment.spec.span;
    // A literal width or precision is kept; a dynamic `{expr}` one is
    // dropped, since no builder produces one.
    if (std::holds_alternative<size_t>(segment.spec.width)) {
      cloned.spec.width = std::get<size_t>(segment.spec.width);
    }
    if (std::holds_alternative<size_t>(segment.spec.precision)) {
      cloned.spec.precision = std::get<size_t>(segment.spec.precision);
    }
  }
  return cloned;
}

auto fragment_builder::build(std::string_view name, std::span<const value> args,
                             std::span<const source_span> arg_spans,
                             source_span span)
    -> std::expected<value, fragment_error> {
  const auto at = [&](size_t i) -> source_span {
    return i < arg_spans.size() ? arg_spans[i] : span;
  };
  // The `expr` fragment argument `i`, or why it is not one.
  const auto expr_arg = [&](size_t i, std::string_view what)
      -> std::expected<const ast::node *, fragment_error> {
    if (args[i].kind != value_kind::expr_fragment ||
        args[i].fragment == nullptr) {
      return fail(at(i), std::string(what));
    }
    return args[i].fragment;
  };
  const auto string_arg =
      [&](size_t i,
          std::string_view what) -> std::expected<std::string, fragment_error> {
    if (args[i].kind != value_kind::string) {
      return fail(at(i), std::string(what));
    }
    return args[i].string;
  };

  if (name == "lit") {
    if (args.size() != 1) {
      return fail(span, "`expr.lit` takes exactly one argument");
    }
    auto lit = ast::make<ast::literal_expr>();
    lit->span = span;
    switch (args[0].kind) {
    case value_kind::integer:
      lit->lit_kind = token_kind::int_lit;
      lit->value = std::to_string(args[0].integer);
      break;
    case value_kind::floating:
      lit->lit_kind = token_kind::float_lit;
      lit->value = std::to_string(args[0].floating);
      break;
    case value_kind::boolean:
      lit->lit_kind =
          args[0].boolean ? token_kind::kw_true : token_kind::kw_false;
      lit->value = args[0].boolean ? "true" : "false";
      break;
    case value_kind::string:
      lit->lit_kind = token_kind::string_lit;
      lit->value = encode_string_literal(args[0].string);
      break;
    default:
      return fail(at(0), "`expr.lit` only supports integer, floating-point, "
                         "boolean, and string values");
    }
    return keep(std::move(lit), value_kind::expr_fragment);
  }

  if (name == "ident") {
    if (args.size() != 1) {
      return fail(span, "`expr.ident` takes exactly one argument");
    }
    auto text = string_arg(0, "`expr.ident` expects a string naming the "
                              "identifier");
    if (!text.has_value()) {
      return std::unexpected(text.error());
    }
    auto ident = ast::make<ast::ident_expr>();
    // `is_variant_ident` tells a `@variant` reference from a plain name by
    // comparing the span's length with the name's, so the span is exactly
    // as long as the name.
    ident->span =
        source_span{.start = span.start,
                    .end = span.start + static_cast<byte_offset>(text->size())};
    ident->name = std::move(*text);
    return keep(std::move(ident), value_kind::expr_fragment);
  }

  if (name == "field") {
    if (args.size() != 2) {
      return fail(span, "`expr.field` takes exactly two arguments: an `expr` "
                        "object and a field-name string");
    }
    auto object = expr_arg(0, "`expr.field`'s first argument must be a "
                              "quoted or constructed `expr` value");
    if (!object.has_value()) {
      return std::unexpected(object.error());
    }
    auto field = string_arg(1, "`expr.field`'s second argument must be a "
                               "string naming the field");
    if (!field.has_value()) {
      return std::unexpected(field.error());
    }
    auto cloned = clone_expr(**object);
    if (cloned == nullptr) {
      return fail(at(0), "this quoted `expr` value's syntax is too complex "
                         "for `expr.field` to embed (only identifiers, field "
                         "access, and literals are supported)");
    }
    auto result = ast::make<ast::field_expr>();
    result->span = span;
    result->object = std::move(cloned);
    result->field_name = std::move(*field);
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "interp_concat") {
    if (args.size() != 2) {
      return fail(span, "`expr.interp_concat` takes exactly two `expr` "
                        "arguments");
    }
    auto result = ast::make<ast::interpolated_string_expr>();
    result->span = span;
    for (size_t i = 0; i < 2; ++i) {
      auto part = expr_arg(i, "`expr.interp_concat`'s arguments must be "
                              "quoted or constructed `expr` values");
      if (!part.has_value()) {
        return std::unexpected(part.error());
      }
      if (auto appended = append_segments(**part, at(i), result->segments);
          !appended.has_value()) {
        return std::unexpected(appended.error());
      }
    }
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "debug") {
    if (args.size() != 1) {
      return fail(span, "`expr.debug` takes exactly one `expr` argument");
    }
    auto shown = expr_arg(0, "`expr.debug`'s argument must be a quoted or "
                             "constructed `expr` value");
    if (!shown.has_value()) {
      return std::unexpected(shown.error());
    }
    auto cloned = clone_expr(**shown);
    if (cloned == nullptr) {
      return fail(at(0), "this quoted `expr` value's syntax is too complex "
                         "for `expr.debug` to embed");
    }
    auto result = ast::make<ast::interpolated_string_expr>();
    result->span = span;
    // One value segment with the `?` type char — what `"{x:?}"` parses to.
    auto segment = ast::interp_segment{};
    segment.is_literal = false;
    segment.value = std::move(cloned);
    segment.has_spec = true;
    segment.spec.type_char = '?';
    result->segments.push_back(std::move(segment));
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "binary") {
    if (args.size() != 3) {
      return fail(span, "`expr.binary` takes exactly three arguments: an "
                        "operator name string and two `expr` operands");
    }
    auto op_name = string_arg(0, "`expr.binary`'s first argument must be a "
                                 "string naming the operator");
    if (!op_name.has_value()) {
      return std::unexpected(op_name.error());
    }
    // Only the operators a derivation needs.
    const auto op = *op_name == "=="    ? ast::binary_op::eq_eq
                    : *op_name == "!="  ? ast::binary_op::bang_eq
                    : *op_name == "and" ? ast::binary_op::logical_and
                    : *op_name == "or"  ? ast::binary_op::logical_or
                                        : std::optional<ast::binary_op>{};
    if (!op.has_value()) {
      return fail(at(0), std::format("`expr.binary` does not recognize the "
                                     "operator `{}` (supported: `==`, `!=`, "
                                     "`and`, `or`)",
                                     *op_name));
    }
    auto lhs = expr_arg(1, "`expr.binary`'s operands must be quoted or "
                           "constructed `expr` values");
    if (!lhs.has_value()) {
      return std::unexpected(lhs.error());
    }
    auto rhs = expr_arg(2, "`expr.binary`'s operands must be quoted or "
                           "constructed `expr` values");
    if (!rhs.has_value()) {
      return std::unexpected(rhs.error());
    }
    auto cloned_lhs = clone_expr(**lhs);
    auto cloned_rhs = clone_expr(**rhs);
    if (cloned_lhs == nullptr || cloned_rhs == nullptr) {
      return fail(span, "one of `expr.binary`'s operands' syntax is too "
                        "complex to embed");
    }
    auto result = ast::make<ast::binary_expr>();
    result->span = span;
    result->op = *op;
    result->lhs = std::move(cloned_lhs);
    result->rhs = std::move(cloned_rhs);
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "call") {
    // `expr.call(callee, arg...)` lets a generated body be a fold — a chain
    // of calls — rather than one flat expression; `derive_ord` needs it.
    // Positional arguments only.
    if (args.empty()) {
      return fail(span, "`expr.call` takes at least one argument: the `expr` "
                        "being called, then its arguments");
    }
    auto callee = expr_arg(0, "`expr.call`'s first argument must be a quoted "
                              "or constructed `expr` value naming what to "
                              "call");
    if (!callee.has_value()) {
      return std::unexpected(callee.error());
    }
    auto cloned_callee = clone_expr(**callee);
    if (cloned_callee == nullptr) {
      return fail(at(0), "this quoted `expr` value's syntax is too complex "
                         "for `expr.call` to embed as a callee");
    }
    auto result = ast::make<ast::call_expr>();
    result->span = span;
    result->callee = std::move(cloned_callee);
    for (size_t i = 1; i < args.size(); ++i) {
      auto arg = expr_arg(i, "`expr.call`'s arguments must be quoted or "
                             "constructed `expr` values");
      if (!arg.has_value()) {
        return std::unexpected(arg.error());
      }
      auto cloned = clone_expr(**arg);
      if (cloned == nullptr) {
        return fail(at(i), "this quoted `expr` value's syntax is too complex "
                           "for `expr.call` to embed as an argument");
      }
      result->args.push_back(
          ast::call_arg{.span = at(i), .value = std::move(cloned)});
    }
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "ctor_pattern") {
    // `@name(prefix0, ..., prefix{arity-1})`, or `@name` for arity 0. Two
    // prefixes let one derivation bind both sides of a comparison.
    if (args.size() != 3) {
      return fail(span, "`expr.ctor_pattern` takes exactly three arguments: "
                        "a variant-name string, a payload-arity integer, and "
                        "a bound-name prefix string");
    }
    auto variant = string_arg(0, "`expr.ctor_pattern`'s first argument must "
                                 "be a string naming the variant");
    if (!variant.has_value()) {
      return std::unexpected(variant.error());
    }
    if (args[1].kind != value_kind::integer || args[1].integer < 0) {
      return fail(at(1), "`expr.ctor_pattern`'s second argument must be a "
                         "non-negative integer naming the payload arity");
    }
    auto prefix = string_arg(2, "`expr.ctor_pattern`'s third argument must "
                                "be a string naming the bound-name prefix");
    if (!prefix.has_value()) {
      return std::unexpected(prefix.error());
    }
    auto ctor = ast::make<ast::constructor_pattern>();
    ctor->span = span;
    ctor->name = std::move(*variant);
    for (int64_t i = 0; i < args[1].integer; ++i) {
      auto binding = ast::make<ast::binding_pattern>();
      binding->span = span;
      binding->name = std::format("{}{}", *prefix, i);
      ctor->args.push_back(std::move(binding));
    }
    return keep(std::move(ctor), value_kind::pattern_fragment);
  }

  if (name == "match_on") {
    // An arm-less `match subject:`; `expr.arm` adds the arms one at a time.
    if (args.size() != 1) {
      return fail(span, "`expr.match_on` takes exactly one `expr` argument: "
                        "the subject being matched");
    }
    auto subject = expr_arg(0, "`expr.match_on`'s argument must be a quoted "
                               "or constructed `expr` value");
    if (!subject.has_value()) {
      return std::unexpected(subject.error());
    }
    auto cloned = clone_expr(**subject);
    if (cloned == nullptr) {
      return fail(at(0), "this quoted `expr` value's syntax is too complex "
                         "for `expr.match_on` to embed as a subject");
    }
    auto result = ast::make<ast::match_expr>();
    result->span = span;
    result->subject = std::move(cloned);
    return keep(std::move(result), value_kind::expr_fragment);
  }

  if (name == "arm") {
    // A new `match` with one more arm, `pattern => body`; the input is
    // untouched, so `body = expr.arm(body, ...)` accumulates.
    if (args.size() != 3) {
      return fail(span, "`expr.arm` takes exactly three arguments: the "
                        "`match` expr being extended, a `pattern`, and the "
                        "arm's body `expr`");
    }
    if (args[0].kind != value_kind::expr_fragment ||
        args[0].fragment == nullptr ||
        args[0].fragment->kind != ast::node_kind::match_expr) {
      return fail(at(0), "`expr.arm`'s first argument must be a `match` expr "
                         "built by `expr.match_on`");
    }
    auto extended = clone_expr(*args[0].fragment);
    if (extended == nullptr) {
      return fail(at(0), "this `match` expr's syntax is too complex for "
                         "`expr.arm` to extend");
    }
    if (args[1].kind != value_kind::pattern_fragment ||
        args[1].fragment == nullptr) {
      return fail(at(1), "`expr.arm`'s second argument must be a `pattern` "
                         "built by `expr.ctor_pattern`");
    }
    auto pattern = clone_pattern(*args[1].fragment);
    if (pattern == nullptr) {
      return fail(at(1), "this `pattern`'s syntax is too complex for "
                         "`expr.arm` to embed");
    }
    auto body = expr_arg(2, "`expr.arm`'s third argument must be a quoted or "
                            "constructed `expr` value");
    if (!body.has_value()) {
      return std::unexpected(body.error());
    }
    auto cloned_body = clone_expr(**body);
    if (cloned_body == nullptr) {
      return fail(at(2), "this quoted `expr` value's syntax is too complex "
                         "for `expr.arm` to embed as an arm body");
    }
    auto &match = ast::cast<ast::match_expr>(*extended);
    ast::match_arm arm;
    arm.span = span;
    arm.pattern = std::move(pattern);
    arm.body_expr = std::move(cloned_body);
    match.arms.push_back(std::move(arm));
    return keep(std::move(extended), value_kind::expr_fragment);
  }

  return fail(span, std::format("`expr.{}` is not a recognized AST-builder "
                                "intrinsic (only `expr.lit`/`expr.ident`/"
                                "`expr.field`/`expr.interp_concat`/"
                                "`expr.debug`/`expr.binary`/`expr.call`/"
                                "`expr.ctor_pattern`/`expr.match_on`/"
                                "`expr.arm` are supported)",
                                name));
}

} // namespace cinder::comptime
