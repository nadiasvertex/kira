#include <algorithm>
#include <array>
#include <iterator>
#include <format>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/comptime/fragments.h"
#include "src/core/adapter_builder.h"
#include "src/parser/ast_traversal.h"
#include "src/parser/operators.h"
#include "src/parser/text_escape.h"
#include "src/semantic/instance_substitution.h"
#include "src/semantic/linear_poly.h"

/// Expressions, places and calls. Each construct reads the record the checker
/// left for it and nothing else.
namespace cinder::core {

using semantic::k_error_type;
using semantic::type_kind;

namespace {

/// The same predicate `check.cpp` and `hir::lower` use: a variant
/// constructor's span still covers its leading `@`.
[[nodiscard]] auto is_variant_ident(const ast::ident_expr &ident) -> bool {
  return ident.span.len() > ident.name.size();
}

[[nodiscard]] auto is_self_param(const ast::param &param) -> bool {
  const auto *binding = ast::as<const ast::binding_pattern>(param.pattern.get());
  return binding != nullptr && binding->name == "self";
}

[[nodiscard]] auto strip_groups(const ast::expr *expr) -> const ast::expr * {
  while (expr != nullptr && expr->kind == ast::node_kind::group_expr) {
    expr = ast::cast<const ast::group_expr>(*expr).inner.get();
  }
  return expr;
}

/// A template's `signature` restated at `args`, one per parameter in
/// `params`. A value parameter's argument (a constant, `n + 1`, or another
/// value parameter) is also substituted into the polynomials that mention
/// it, as an instance's answers are.
[[nodiscard]] auto template_signature(semantic::type_table &types,
                                      type_id signature,
                                      const std::vector<type_id> &params,
                                      const std::vector<type_id> &args)
    -> type_id {
  auto subst = semantic::detail::instance_subst{};
  for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
    subst.params.emplace(params[i], args[i]);
    const auto var =
        semantic::poly_param_var(types.entry(params[i]).name, params[i]);
    const auto &arg = types.entry(args[i]);
    if (arg.kind == type_kind::const_value_kind ||
        arg.kind == type_kind::symbolic_value_kind) {
      subst.values.emplace(var, arg.value);
    } else if (arg.kind == type_kind::type_param_kind) {
      subst.values.emplace(var, semantic::poly_variable(semantic::poly_param_var(
                                    arg.name, args[i])));
    }
  }
  return semantic::detail::substitute_type(
      types, [](type_id id) -> type_id { return id; }, subst, signature);
}

} // namespace

auto kind_name(ast::node_kind kind) -> std::string {
  using k = ast::node_kind;
  switch (kind) {
  case k::let_stmt: return "let_stmt";
  case k::var_stmt: return "var_stmt";
  case k::assign_stmt: return "assign_stmt";
  case k::expr_stmt: return "expr_stmt";
  case k::return_stmt: return "return_stmt";
  case k::break_stmt: return "break_stmt";
  case k::continue_stmt: return "continue_stmt";
  case k::if_stmt: return "if_stmt";
  case k::while_stmt: return "while_stmt";
  case k::scope_stmt: return "scope_stmt";
  case k::for_stmt: return "for_stmt";
  case k::match_stmt: return "match_stmt";
  case k::crew_stmt: return "crew_stmt";
  case k::asm_stmt: return "asm_stmt";
  case k::splice_stmt: return "splice_stmt";
  case k::static_decl: return "static_decl";
  case k::func_decl: return "func_decl";
  case k::ident_expr: return "ident_expr";
  case k::literal_expr: return "literal_expr";
  case k::binary_expr: return "binary_expr";
  case k::unary_expr: return "unary_expr";
  case k::call_expr: return "call_expr";
  case k::index_expr: return "index_expr";
  case k::field_expr: return "field_expr";
  case k::cast_expr: return "cast_expr";
  case k::try_expr: return "try_expr";
  case k::or_else_expr: return "or_else_expr";
  case k::tuple_expr: return "tuple_expr";
  case k::array_expr: return "array_expr";
  case k::struct_expr: return "struct_expr";
  case k::lambda_expr: return "lambda_expr";
  case k::match_expr: return "match_expr";
  case k::if_expr: return "if_expr";
  case k::for_expr: return "for_expr";
  case k::await_expr: return "await_expr";
  case k::async_expr: return "async_expr";
  case k::yield_expr: return "yield_expr";
  case k::par_expr: return "par_expr";
  case k::race_expr: return "race_expr";
  case k::crew_expr: return "crew_expr";
  case k::on_expr: return "on_expr";
  case k::block_expr: return "block_expr";
  case k::quote_expr: return "quote_expr";
  case k::splice_expr: return "splice_expr";
  case k::static_expr: return "static_expr";
  case k::module_path_expr: return "module_path_expr";
  case k::group_expr: return "group_expr";
  case k::where_expr: return "where_expr";
  case k::interpolated_string_expr: return "interpolated_string_expr";
  case k::wildcard_pattern: return "wildcard_pattern";
  case k::literal_pattern: return "literal_pattern";
  case k::binding_pattern: return "binding_pattern";
  case k::constructor_pattern: return "constructor_pattern";
  case k::tuple_pattern: return "tuple_pattern";
  case k::struct_pattern: return "struct_pattern";
  case k::array_pattern: return "array_pattern";
  case k::range_pattern: return "range_pattern";
  case k::ref_pattern: return "ref_pattern";
  case k::or_pattern: return "or_pattern";
  case k::group_pattern: return "group_pattern";
  default:
    return std::format("node_kind {}", static_cast<int>(kind));
  }
}

// ==========================================================================
//  Types
// ==========================================================================

namespace {

/// The refusal of a type the checker left unsettled, as the old lowerer
/// words it; `holding` says which part is open, when only part is.
[[nodiscard]] auto unsettled(std::string_view holding, source_span span)
    -> std::unexpected<failure> {
  return fail_feature(
      "unsettled type",
      std::format("no concrete checked type is available for this node{}; "
                  "lowering only accepts fully type-checked, fully-annotated "
                  "code (see spec/todo.md)",
                  holding.empty() ? std::string{}
                                  : std::format(" ({})", holding)),
      span);
}

} // namespace

auto body_builder::settle(type_id type, source_span span) -> result<type_id> {
  if (type == k_unknown_type || type == k_error_type) {
    return unsettled({}, span);
  }
  auto settled = type;
  if (checked_.types.entry(settled).kind == type_kind::existential_kind) {
    settled = checked_.types.entry(settled).result;
  }
  settled = checked_.types.erase_refinements(settled);
  // Every type the core may not hold, at any depth (verifier T1, T2), and
  // any parameter the function being built does not hold open (T3).
  auto pending = std::vector<type_id>{settled};
  auto seen = 0;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (++seen > 4096) {
      return fail("unsettled type", "the type nests without end", span);
    }
    const auto &entry = checked_.types.entry(id);
    if (entry.kind == type_kind::type_var_kind && !open_params_.empty()) {
      // A template leaves a leaf open where only an instance's types solve
      // it (a call of another template, a literal given a parameter's type).
      return fail("open inference variable (deferred to instances)",
                  std::format("`{}` holds `{}`", checked_.types.display(settled),
                              checked_.types.display(id)),
                  span);
    }
    switch (entry.kind) {
    case type_kind::unknown_kind:
    case type_kind::error_kind:
    case type_kind::type_var_kind:
    case type_kind::type_param_kind:
      if (open_params_.contains(id)) {
        continue;
      }
      return unsettled(std::format("`{}` holds `{}`",
                                   checked_.types.display(settled),
                                   checked_.types.display(id)),
                       span);
    case type_kind::param_app_kind:
      if (open_params_.contains(entry.result)) {
        for (const auto arg : entry.args) {
          pending.push_back(arg);
        }
        continue;
      }
      return fail("unsettled type",
                  std::format("`{}` holds `{}`", checked_.types.display(settled),
                              checked_.types.display(id)),
                  span);
    case type_kind::symbolic_value_kind:
      if (std::ranges::all_of(entry.value.terms, [&](const auto &term) {
            const auto param = semantic::poly_var_identity(term.var);
            return param.has_value() && open_params_.contains(*param);
          })) {
        continue;
      }
      return fail("unsettled type",
                  std::format("`{}` holds `{}`", checked_.types.display(settled),
                              checked_.types.display(id)),
                  span);
    case type_kind::existential_kind:
    case type_kind::ctor_ref_kind:
    case type_kind::refinement_kind:
      return fail("unsettled type",
                  std::format("`{}` holds `{}`", checked_.types.display(settled),
                              checked_.types.display(id)),
                  span);
    default:
      break;
    }
    for (const auto arg : entry.args) {
      pending.push_back(arg);
    }
    if (entry.result != k_unknown_type) {
      pending.push_back(entry.result);
    }
  }
  return settled;
}

auto body_builder::is_open(type_id type) const -> bool {
  if (open_params_.empty()) {
    return false;
  }
  auto pending = std::vector<type_id>{type};
  auto seen = 0;
  while (!pending.empty() && ++seen < 4096) {
    const auto &entry = checked_.types.entry(pending.back());
    pending.pop_back();
    if (entry.kind == type_kind::type_param_kind ||
        entry.kind == type_kind::param_app_kind ||
        entry.kind == type_kind::symbolic_value_kind) {
      return true;
    }
    for (const auto arg : entry.args) {
      pending.push_back(arg);
    }
    if (entry.result != k_unknown_type) {
      pending.push_back(entry.result);
    }
  }
  return false;
}

auto body_builder::is_open_operand(const ast::expr &operand) const -> bool {
  const auto found = checked_.node_types.find(&operand);
  return found != checked_.node_types.end() && is_open_scalar(found->second);
}

auto body_builder::is_open_scalar(type_id type) const -> bool {
  // A raw pointer compares and offsets the same way whatever it points at.
  return is_open(type) &&
         checked_.types.entry(strip_refs(type)).kind != type_kind::ptr_kind;
}

auto body_builder::open_generic_params(std::vector<type_id> params,
                                       const template_table *templates)
    -> void {
  open_params_ = {params.begin(), params.end()};
  generic_params_ = std::move(params);
  templates_ = templates;
}

auto body_builder::type_of(const ast::node &node) -> result<type_id> {
  const auto found = checked_.node_types.find(&node);
  if (found == checked_.node_types.end()) {
    return fail("missing record: node_types",
                std::format("no type for a {}", kind_name(node.kind)),
                node.span);
  }
  return settle(found->second, node.span);
}

auto body_builder::strip_refs(type_id type) const -> type_id {
  while (checked_.types.entry(type).kind == type_kind::ref_kind) {
    type = checked_.types.entry(type).result;
  }
  return type;
}

auto body_builder::task_of(type_id body) -> type_id {
  auto &types = checked_.types;
  const auto &entry = types.entry(body);
  const auto fallible = entry.kind == type_kind::builtin_generic_kind &&
                        entry.name == "result" && entry.args.size() == 2;
  const auto value = fallible ? entry.args[0] : body;
  const auto error = fallible ? entry.args[1] : types.builtin("never");
  return types.builtin_generic("task", {value, error, types.builtin("io")});
}

auto body_builder::await_of(const ast::await_expr &awaited)
    -> result<ptr<expr>> {
  if (awaited.is_yield || awaited.operand == nullptr) {
    // A task body: an `async def`, the entry `async def main`, or an
    // `async:`/`on(...)` block. The checker accepts `await yield` anywhere,
    // but an `await` outside an `async` function is an error (chapter 26).
    const auto in_async = lambdas_.empty()
                              ? decl_ != nullptr && decl_->modifiers.is_async
                              : lambdas_.back().task_key != nullptr;
    if (!in_async) {
      return fail_feature("await_expr",
                          "`await yield` can only be used in an `async def` "
                          "or an `async:` block",
                          awaited.span);
    }
    return intrinsic_call("task_yield", unit_type(), {}, {}, awaited.span);
  }
  if (auto form = await_task_form(awaited)) {
    return std::move(*form);
  }
  CORE_TRY(type, type_of(awaited));
  CORE_TRY(task_type, type_of(*awaited.operand));
  CORE_TRY(task, value_as(*awaited.operand, strip_refs(task_type)));
  auto made = make_node<await_expr>(awaited.span, type);
  made->task = std::move(task);
  return ptr<expr>(std::move(made));
}

auto body_builder::is_ref(type_id type) const -> bool {
  return checked_.types.entry(type).kind == type_kind::ref_kind;
}

auto body_builder::unit_type() -> type_id {
  return checked_.types.builtin("unit");
}

auto body_builder::signature_of(const ast::func_decl &decl)
    -> result<signature> {
  if (const auto found = signatures_.find(&decl); found != signatures_.end()) {
    return found->second;
  }
  auto sig = signature{};
  for (std::size_t i = 0; i < decl.params.size(); ++i) {
    const auto &param = decl.params[i];
    if (param.pattern == nullptr) {
      return fail("param", "a parameter has no pattern", param.span);
    }
    CORE_TRY(type, type_of(*param.pattern));
    if (i == 0 && is_self_param(param)) {
      const auto &owners = checked_.ownership.consuming_receivers;
      const auto tmpl = checked_.ownership.instance_templates.find(&decl);
      const auto consuming =
          owners.contains(&decl) ||
          (tmpl != checked_.ownership.instance_templates.end() &&
           owners.contains(tmpl->second));
      if (consuming) {
        type = strip_refs(type);
        sig.receiver = receiver_mode::consuming;
      } else if (is_ref(type)) {
        sig.receiver = checked_.types.entry(type).is_mut
                           ? receiver_mode::exclusive
                           : receiver_mode::shared;
      } else {
        sig.receiver = receiver_mode::by_value;
      }
    }
    sig.params.push_back(type);
  }
  if (decl.return_type != nullptr) {
    CORE_TRY(type, type_of(*decl.return_type));
    sig.result = type;
  } else {
    const auto found = checked_.inferred_return_types.find(&decl);
    if (found == checked_.inferred_return_types.end()) {
      return fail("missing record: inferred_return_types",
                  std::format("`{}` has no return type", decl.name), decl.span);
    }
    if (found->second == k_unknown_type || found->second == k_error_type) {
      return fail_feature(
          "unsettled type",
          std::format("function `{}` has no declared return type, and none "
                      "could be inferred from its body",
                      decl.name),
          decl.span);
    }
    CORE_TRY(type, settle(found->second, decl.span));
    sig.result = type;
  }
  sig.body = sig.result;
  // The entry `async def main()` is called by the runtime, not awaited, and
  // keeps the type it declares (the checker's `signature_return_type`).
  const auto is_entry_main =
      decl.name == "main" && decl.params.empty() && decl.type_params.empty();
  if (decl.modifiers.is_async &&
      (decl.modifiers.is_generator ||
       (decl.return_type == nullptr && !is_entry_main))) {
    return fail_feature(
        "async def",
        std::format("`async def {}` needs a declared return type, and cannot "
                    "be a generator or have a `post` condition yet",
                    decl.name),
        decl.span);
  }
  if (decl.modifiers.is_async && !is_entry_main) {
    sig.coroutine = coroutine_kind::task;
    sig.result = task_of(sig.body);
  } else if (decl.modifiers.is_generator) {
    const auto &entry = checked_.types.entry(sig.result);
    if (entry.kind != type_kind::builtin_generic_kind ||
        entry.name != "generator" || entry.args.size() != 1) {
      return fail("generator def",
                  std::format("a generator typed `{}`",
                              checked_.types.display(sig.result)),
                  decl.span);
    }
    sig.coroutine = coroutine_kind::generator;
    sig.yield_type = entry.args[0];
    sig.body = unit_type();
  }
  sig.fn_type = checked_.types.fn_of(sig.params, sig.result);
  signatures_.emplace(&decl, sig);
  return sig;
}

// ==========================================================================
//  Scopes
// ==========================================================================

auto body_builder::declare(const std::string &name, type_id type,
                           bool writable, bool pinned) -> symbol_id {
  const auto symbol = mint();
  scopes_.back().insert_or_assign(name, local_info{.symbol = symbol,
                                                   .type = type,
                                                   .writable = writable,
                                                   .pinned = pinned});
  return symbol;
}

auto body_builder::access_of(const place &p) const -> access_state {
  auto state = access_state{};
  switch (p.root.kind) {
  case place_root::origin::local:
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto found = std::ranges::find_if(*it, [&](const auto &entry) {
        return entry.second.symbol == p.root.symbol;
      });
      if (found != it->end()) {
        state.writable = found->second.writable;
        state.movable = !found->second.pinned;
        break;
      }
    }
    break;
  case place_root::origin::global:
    state.movable = false;
    break;
  case place_root::origin::temporary:
    break;
  }
  auto before = p.root.type;
  for (const auto &step : p.path) {
    walk_access(state, before, step);
    before = step.type;
  }
  return state;
}

auto body_builder::walk_access(access_state &state, type_id before,
                               const projection &step) const -> void {
  const auto &entry = checked_.types.entry(before);
  switch (step.kind) {
  case projection::step::deref:
    state.writable = entry.is_mut;
    if (entry.kind == type_kind::ref_kind) {
      state.movable = false;
      state.through_ref = true;
    }
    return;
  case projection::step::element:
    state.movable = false;
    if (entry.kind == type_kind::ptr_kind) {
      state.writable = entry.is_mut;
    } else if (entry.kind == type_kind::builtin_generic_kind) {
      state.writable = entry.name == "slice_mut" || entry.name == "uninit";
    } else if (entry.kind != type_kind::array_kind) {
      state.writable = false;
    }
    return;
  default:
    return;
  }
}

auto body_builder::lookup(const std::string &name)
    -> std::optional<local_info> {
  for (auto depth = scopes_.size(); depth-- > 0;) {
    const auto found = scopes_[depth].find(name);
    if (found == scopes_[depth].end()) {
      continue;
    }
    auto info = found->second;
    // Each lambda between the use and the declaration captures the name,
    // outermost first, so an inner lambda captures the outer's capture.
    for (auto &frame : lambdas_) {
      if (frame.scope_base > depth) {
        info = capture_into(frame, name, info);
      }
    }
    return info;
  }
  return std::nullopt;
}

// ==========================================================================
//  Places
// ==========================================================================

auto body_builder::use_of(place from, source_span span) -> ptr<expr> {
  const auto type = from.type();
  auto read = make_node<use>(span, type);
  read->how = checked_.is_copy(type) ? use::mode::copy : use::mode::move;
  read->from = std::move(from);
  return read;
}

auto body_builder::temporary(ptr<expr> value) -> place {
  auto p = place{};
  p.root.kind = place_root::origin::temporary;
  p.root.type = value->type;
  p.root.value = std::move(value);
  return p;
}

auto body_builder::place_from_value(ptr<expr> value) -> place {
  if (value->kind == node_kind::use) {
    return std::move(static_cast<use &>(*value).from);
  }
  return temporary(std::move(value));
}

namespace {

[[nodiscard]] auto make_step(projection::step kind, type_id type)
    -> projection {
  auto step = projection{};
  step.kind = kind;
  step.type = type;
  return step;
}

} // namespace

auto body_builder::add_deref(place &p) -> void {
  const auto &entry = checked_.types.entry(p.type());
  p.path.push_back(make_step(projection::step::deref, entry.result));
}

namespace {

/// A struct field or tuple element of `p`, from the type's shape.
[[nodiscard]] auto project_member(semantic::checked_types &checked, place &p,
                                  const std::string &name, source_span span)
    -> result<void> {
  const auto type = p.type();
  const auto &entry = checked.types.entry(type);
  if (entry.kind == type_kind::tuple_kind) {
    const auto index = semantic::tuple_index_of(name);
    if (!index.has_value() || *index >= entry.args.size()) {
      return fail("field_expr",
                  std::format("`{}` is not an element of `{}`", name,
                              checked.types.display(type)),
                  span);
    }
    auto step = make_step(projection::step::tuple_element, entry.args[*index]);
    step.index = *index;
    p.path.push_back(std::move(step));
    return {};
  }
  const auto *shape = find_shape(checked, type);
  if (shape == nullptr) {
    return fail("missing record: shapes",
                std::format("no shape for `{}`", checked.types.display(type)),
                span);
  }
  for (std::size_t i = 0; i < shape->fields.size(); ++i) {
    if (shape->fields[i].name == name) {
      auto step =
          make_step(projection::step::field, shape->fields[i].type);
      step.name = name;
      step.index = i;
      p.path.push_back(std::move(step));
      return {};
    }
  }
  return fail("missing record: shapes",
              std::format("`{}` has no field `{}`",
                          checked.types.display(type), name),
              span);
}

} // namespace

auto body_builder::apply_deref_steps(place p,
                                     const std::vector<semantic::deref_step>
                                         &steps,
                                     source_span span) -> result<place> {
  for (const auto &step : steps) {
    CORE_TRY(target, callee_of(step.callee, span));
    const auto &sig = checked_.types.entry(target.signature);
    if (sig.args.size() != 1) {
      return fail("missing record: deref_adjustments",
                  "a `deref` step's method does not take one parameter", span);
    }
    auto args = ptr_vec<expr>{};
    args.push_back(receiver_argument(std::move(p), sig.args[0], span));
    auto called = direct_call(std::move(target), std::move(args), span);
    p = temporary(std::move(called));
    if (!is_ref(p.type())) {
      return fail("missing record: deref_adjustments",
                  "a `deref` method does not return a reference", span);
    }
    add_deref(p);
  }
  return p;
}

auto body_builder::receiver_place(const ast::expr &source,
                                  place_access access) -> result<place> {
  CORE_TRY(p, place_of(source, access));
  if (access == place_access::mutate) {
    // A part written through a handle reaches it through `deref_mut`.
    if (const auto steps = checked_.deref_mut_adjustments.find(&source);
        steps != checked_.deref_mut_adjustments.end()) {
      return apply_deref_steps(std::move(p), steps->second, source.span);
    }
  }
  if (const auto steps = checked_.deref_adjustments.find(&source);
      steps != checked_.deref_adjustments.end()) {
    return apply_deref_steps(std::move(p), steps->second, source.span);
  }
  return p;
}

auto body_builder::place_of(const ast::expr &source, place_access access)
    -> result<place> {
  if (const auto rewritten = checked_.spliced_fragments.find(&source);
      rewritten != checked_.spliced_fragments.end()) {
    if (const auto *fragment = ast::as<const ast::expr>(rewritten->second)) {
      return place_of(*fragment, access);
    }
  }
  if (source.has_error) {
    return fail("error node", "the expression carries a parse error",
                source.span);
  }
  if (const auto *folded = static_fold(source)) {
    CORE_TRY(value, value_of(*folded));
    return temporary(std::move(value));
  }
  // A reified static is a place; the other folded statics are values.
  if (const auto global = checked_.static_global_refs.find(&source);
      global != checked_.static_global_refs.end() &&
      !checked_.lowering.static_const_values.contains(&source) &&
      !checked_.lowering.static_struct_values.contains(&source)) {
    const auto owner =
        checked_.lowering.static_global_owners.find(global->second);
    if (owner == checked_.lowering.static_global_owners.end()) {
      return fail("missing record: static_global_owners",
                  std::format("no owner for `{}`", global->second), source.span);
    }
    CORE_TRY(type, type_of(source));
    // A read that takes a value of its own is the static's value built
    // again, held as a temporary; any other reads the global in place.
    if (fresh_static_read(source)) {
      CORE_TRY(value, static_global_value(global->second, source.span));
      return temporary(std::move(value));
    }
    auto p = place{};
    p.root.kind = place_root::origin::global;
    p.root.name = global->second;
    p.root.module = owner->second;
    p.root.type = type;
    return p;
  }
  if (auto fragment = static_fragment_of(source)) {
    CORE_TRY(value, std::move(*fragment));
    return temporary(std::move(value));
  }
  switch (source.kind) {
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(source);
    if (group.inner == nullptr) {
      return fail("group_expr", "empty parentheses", source.span);
    }
    return place_of(*group.inner, access);
  }
  case ast::node_kind::ident_expr: {
    const auto &ident = ast::cast<const ast::ident_expr>(source);
    const auto is_value =
        is_variant_ident(ident) ||
        checked_.lowering.static_const_values.contains(&source) ||
        checked_.lowering.static_struct_values.contains(&source) ||
        checked_.lowering.resolved_fn_values.contains(&source);
    if (const auto copy = checked_.ownership.generic_lambda_refs.find(&source);
        copy != checked_.ownership.generic_lambda_refs.end()) {
      // A use of a generic lambda names the local of the copy it was
      // checked against.
      const auto name = copy_name(ident.name, *copy->second);
      const auto local = lookup(name);
      if (!local.has_value()) {
        return fail("missing record: generic_lambda_instances",
                    std::format("no copy of `{}` is bound here", ident.name),
                    source.span);
      }
      auto p = place{};
      p.root.kind = place_root::origin::local;
      p.root.symbol = local->symbol;
      p.root.name = name;
      p.root.type = local->type;
      return p;
    }
    if (!is_value) {
      if (const auto local = lookup(ident.name)) {
        auto p = place{};
        p.root.kind = place_root::origin::local;
        p.root.symbol = local->symbol;
        p.root.name = ident.name;
        p.root.type = local->type;
        return p;
      }
    }
    break;
  }
  case ast::node_kind::field_expr: {
    const auto &field = ast::cast<const ast::field_expr>(source);
    if (checked_.lowering.static_const_values.contains(&source) ||
        checked_.lowering.static_struct_values.contains(&source)) {
      break;
    }
    if (field.object == nullptr) {
      return fail("field_expr", "a field access has no object", source.span);
    }
    if (!field.generic_args.empty()) {
      return fail("field_expr", "generic method arguments", source.span);
    }
    CORE_TRY(p, receiver_place(*field.object, access));
    while (is_ref(p.type())) {
      add_deref(p);
    }
    CORE_CHECK(project_member(checked_, p, field.field_name, source.span));
    return p;
  }
  case ast::node_kind::module_path_expr: {
    const auto &path = ast::cast<const ast::module_path_expr>(source);
    if (checked_.lowering.static_const_values.contains(&source) ||
        checked_.lowering.static_struct_values.contains(&source) ||
        checked_.lowering.resolved_fn_values.contains(&source)) {
      break;
    }
    const auto value_path = checked_.value_path_types.find(&path);
    if (value_path == checked_.value_path_types.end()) {
      if (checked_.lowering.unreadable_static_refs.contains(&source) ||
          checked_.lowering.static_fragment_refs.contains(&source)) {
        return unreadable_static(source);
      }
      return fail("missing record: value_path_types",
                  "a module path that names no value", source.span);
    }
    const auto root = lookup(path.segments[0]);
    if (!root.has_value()) {
      auto written = std::string{};
      for (const auto &segment : path.segments) {
        written += written.empty() ? segment : "." + segment;
      }
      return fail_feature(
          "module_path_expr",
          std::format("`{}` reads a part of the `static` `{}` that is a "
                      "struct or a sum; reading such a part is not "
                      "implemented yet. A whole struct-valued `static` and "
                      "its scalar fields can be read (see spec/todo.md, "
                      "item 26)",
                      written, path.segments[0]),
          source.span);
    }
    if (value_path->second.size() != path.segments.size()) {
      return fail("module_path_expr",
                  std::format("`{}` is not rooted at a local",
                              path.segments[0]),
                  source.span);
    }
    auto p = place{};
    p.root.kind = place_root::origin::local;
    p.root.symbol = root->symbol;
    p.root.name = path.segments[0];
    p.root.type = root->type;
    const auto derefs = checked_.path_deref_adjustments.find(&path);
    const auto mut_derefs = access == place_access::mutate
                                ? checked_.path_deref_mut_adjustments.find(&path)
                                : checked_.path_deref_mut_adjustments.end();
    for (std::size_t i = 1; i < path.segments.size(); ++i) {
      // A segment written through a handle reaches it through `deref_mut`.
      const auto *steps =
          mut_derefs != checked_.path_deref_mut_adjustments.end() &&
                  i < mut_derefs->second.size() && !mut_derefs->second[i].empty()
              ? &mut_derefs->second[i]
          : derefs != checked_.path_deref_adjustments.end() &&
                  i < derefs->second.size() && !derefs->second[i].empty()
              ? &derefs->second[i]
              : nullptr;
      if (steps != nullptr) {
        CORE_TRY(through, apply_deref_steps(std::move(p), *steps, source.span));
        p = std::move(through);
      }
      while (is_ref(p.type())) {
        add_deref(p);
      }
      CORE_CHECK(project_member(checked_, p, path.segments[i], source.span));
    }
    return p;
  }
  case ast::node_kind::unary_expr: {
    const auto &un = ast::cast<const ast::unary_expr>(source);
    if (un.op != ast::unary_op::deref || un.operand == nullptr) {
      break;
    }
    CORE_TRY(p, place_of(*un.operand, access));
    if (access == place_access::mutate) {
      if (const auto steps =
              checked_.deref_mut_adjustments.find(un.operand.get());
          steps != checked_.deref_mut_adjustments.end()) {
        // `*g = v` on a handle with `deref_mut` writes through
        // `g.deref_mut()`.
        return apply_deref_steps(std::move(p), steps->second, source.span);
      }
    }
    if (const auto steps = checked_.deref_adjustments.find(un.operand.get());
        steps != checked_.deref_adjustments.end() && !steps->second.empty()) {
      // `*h` on a `deref` handle reads through `h.deref()`, whose result is
      // already followed by its `deref` step.
      return apply_deref_steps(std::move(p), steps->second, source.span);
    }
    const auto kind = checked_.types.entry(p.type()).kind;
    if (kind != type_kind::ref_kind && kind != type_kind::ptr_kind) {
      return fail("unary_expr",
                  std::format("`*` of `{}`, which is not a reference",
                              checked_.types.display(p.type())),
                  source.span);
    }
    add_deref(p);
    return p;
  }
  case ast::node_kind::index_expr: {
    const auto &index = ast::cast<const ast::index_expr>(source);
    if (index.object == nullptr || index.index == nullptr) {
      return fail("index_expr", "an index has no object or key", source.span);
    }
    CORE_TRY(type, type_of(source));
    if (access == place_access::mutate) {
      if (const auto by_mut = checked_.lowering.index_mut_dispatches.find(&index);
          by_mut != checked_.lowering.index_mut_dispatches.end()) {
        // A written element is reached through `*at_mut(&mut v, i)`; the
        // receiver is itself a written place.
        CORE_TRY(called, dispatch_call(by_mut->second, *index.object,
                                       {index.index.get()}, source.span));
        if (!is_ref(called->type) || !checked_.types.entry(called->type).is_mut) {
          return fail("index_expr",
                      std::format("`at_mut` returns `{}`, not a `&mut`",
                                  checked_.types.display(called->type)),
                      source.span);
        }
        auto p = temporary(std::move(called));
        add_deref(p);
        return p;
      }
    }
    if (const auto by_ref = checked_.lowering.index_ref_dispatches.find(&index);
        by_ref != checked_.lowering.index_ref_dispatches.end() &&
        !checked_.is_copy(type)) {
      // The element is read in place, through `*at_ref(v, i)`.
      CORE_TRY(called, dispatch_call(by_ref->second, *index.object,
                                     {index.index.get()}, source.span));
      if (!is_ref(called->type)) {
        return fail("index_expr",
                    std::format("`at_ref` returns `{}`, not a reference",
                                checked_.types.display(called->type)),
                    source.span);
      }
      auto p = temporary(std::move(called));
      add_deref(p);
      return p;
    }
    if (const auto found = checked_.lowering.index_dispatches.find(&index);
        found != checked_.lowering.index_dispatches.end()) {
      CORE_TRY(called, dispatch_call(found->second, *index.object,
                                     {index.index.get()}, source.span));
      auto p = temporary(std::move(called));
      // An `at` that returns a reference to the element is read through
      // it; one whose element is itself a reference (`list[&T]`) returns
      // the element, the expression's own type.
      if (is_ref(p.type()) && p.type() != type) {
        add_deref(p);
      }
      return p;
    }
    if (const auto *key = strip_groups(index.index.get());
        key != nullptr && key->kind == ast::node_kind::binary_expr &&
        ast::is_range(ast::cast<const ast::binary_expr>(*key).op)) {
      CORE_TRY(view,
               range_index(index, ast::cast<const ast::binary_expr>(*key),
                           type));
      return temporary(std::move(view));
    }
    CORE_TRY(p, place_of(*index.object, access));
    while (is_ref(p.type())) {
      add_deref(p);
    }
    CORE_TRY(key_type, type_of(*index.index));
    CORE_TRY(subscript, value_as(*index.index, strip_refs(key_type)));
    auto step = make_step(projection::step::element, type);
    step.subscript = std::move(subscript);
    step.in_bounds = checked_.lowering.proven_in_bounds.contains(&index);
    p.path.push_back(std::move(step));
    return p;
  }
  default:
    break;
  }
  CORE_TRY(value, value_of(source));
  return temporary(std::move(value));
}

// ==========================================================================
//  Values
// ==========================================================================

auto body_builder::coerce(ptr<expr> value, type_id need)
    -> result<ptr<expr>> {
  if (value == nullptr || value->type == need || need == k_unknown_type) {
    return value;
  }
  const auto have = value->type;
  const auto span = value->span;
  const auto &need_entry = checked_.types.entry(need);
  const auto &have_entry = checked_.types.entry(have);
  if (need_entry.kind == type_kind::ref_kind) {
    const auto inner = need_entry.result;
    const auto is_mut = need_entry.is_mut;
    if (have == inner) {
      auto lent = make_node<borrow>(span, need);
      lent->of = place_from_value(std::move(value));
      lent->is_mut = is_mut;
      lent->inserted = true;
      return lent;
    }
    if (have_entry.kind == type_kind::ref_kind && have_entry.is_mut &&
        !is_mut && have_entry.result == inner) {
      auto lent = make_node<borrow>(span, need);
      lent->of = place_from_value(std::move(value));
      add_deref(lent->of);
      lent->inserted = true;
      return lent;
    }
  }
  if (have_entry.kind == type_kind::ref_kind && strip_refs(have) == need) {
    auto p = place_from_value(std::move(value));
    while (p.type() != need && is_ref(p.type())) {
      add_deref(p);
    }
    return use_of(std::move(p), span);
  }
  // `*mut T` where `*T` is expected: a pointer-to-pointer cast (E3).
  if (have_entry.kind == type_kind::ptr_kind &&
      need_entry.kind == type_kind::ptr_kind && have_entry.is_mut &&
      !need_entry.is_mut && have_entry.result == need_entry.result) {
    auto made = make_node<cast>(span, need);
    made->operand = std::move(value);
    return ptr<expr>(std::move(made));
  }
  // `never` fits anywhere; so does a value whose type has not settled, which
  // `settle` reports where it is used.
  if (have_entry.kind == type_kind::builtin_kind && have_entry.name == "never") {
    return value;
  }
  return fail("implicit conversion",
              std::format("`{}` used as `{}`", checked_.types.display(have),
                          checked_.types.display(need)),
              span);
}

auto body_builder::value_as(const ast::expr &source, type_id need)
    -> result<ptr<expr>> {
  CORE_TRY(value, value_of(source));
  return coerce(std::move(value), need);
}

auto body_builder::bool_literal(bool value, source_span span) -> ptr<expr> {
  auto lit = make_node<literal>(span, checked_.types.bool_type());
  lit->token = value ? token_kind::kw_true : token_kind::kw_false;
  lit->spelling = value ? "true" : "false";
  return lit;
}

auto body_builder::literal_of(const ast::literal_expr &lit)
    -> result<ptr<expr>> {
  CORE_TRY(type, type_of(lit));
  if (is_open(type)) {
    return fail("open literal type (deferred to instances)",
                std::format("a literal typed `{}` is given a type per instance",
                            checked_.types.display(type)),
                lit.span);
  }
  auto made = make_node<literal>(lit.span, type);
  made->token = lit.lit_kind;
  made->spelling = lit.value;
  made->origin = &lit;
  return ptr<expr>(std::move(made));
}

auto body_builder::static_value(const ast::expr &ref)
    -> std::optional<result<ptr<expr>>> {
  if (const auto it = checked_.lowering.static_const_values.find(&ref);
      it != checked_.lowering.static_const_values.end()) {
    return literal_of(*it->second);
  }
  if (const auto it = checked_.lowering.static_struct_values.find(&ref);
      it != checked_.lowering.static_struct_values.end()) {
    return value_of(*it->second);
  }
  if (const auto global = checked_.static_global_refs.find(&ref);
      global != checked_.static_global_refs.end()) {
    // A read the reader owns is the static's value built again.
    if (fresh_static_read(ref)) {
      return static_global_value(global->second, ref.span);
    }
    auto p = place_of(ref);
    if (!p.has_value()) {
      return std::unexpected(p.error());
    }
    if (checked_.is_copy(p->type())) {
      return use_of(std::move(*p), ref.span);
    }
    // A static owns its value; a read that takes one gets a fresh copy of
    // the recorded value, as lowering builds it.
    return static_global_value(global->second, ref.span);
  }
  if (auto fragment = static_fragment_of(ref)) {
    return std::move(*fragment);
  }
  if (checked_.lowering.unreadable_static_refs.contains(&ref)) {
    return unreadable_static(ref);
  }
  return std::nullopt;
}

auto body_builder::unreadable_static(const ast::expr &ref) const
    -> std::unexpected<failure> {
  auto name = std::string{};
  if (const auto found = checked_.lowering.unreadable_static_refs.find(&ref);
      found != checked_.lowering.unreadable_static_refs.end()) {
    name = found->second;
  } else if (const auto fragment =
                 checked_.lowering.static_fragment_refs.find(&ref);
             fragment != checked_.lowering.static_fragment_refs.end()) {
    name = fragment->second->name;
  }
  const auto type = checked_.node_types.find(&ref);
  const auto shown = type != checked_.node_types.end()
                         ? checked_.types.display(type->second)
                         : std::string{"unknown"};
  return fail_feature(
      "unreadable static",
      std::format(
          "`{}` is a `static let` of type `{}`, and run-time code cannot "
          "read a static of that type yet — only numbers, booleans, "
          "strings, and arrays, lists, tuples, structs, and variants built "
          "from those can be; read it from compile-time code (a `static "
          "def`, `static if`, or another `static let`) instead, or make it "
          "an ordinary `let` inside the function (see spec/todo.md)",
          name, shown),
      ref.span);
}

auto body_builder::fresh_static_read(const ast::expr &ref) -> bool {
  return fresh_static_reads_ || owned_static_reads().contains(&ref);
}

auto body_builder::static_global_value(const std::string &name,
                                       source_span span) -> result<ptr<expr>> {
  // The record `static_global::value` is built from.
  for (const auto &[decl, def] : checked_.lowering.static_global_defs) {
    if (def.name == name) {
      return reified(def.value, span);
    }
  }
  return fail("missing record: static_global_defs",
              std::format("no value for `{}`", name), span);
}

auto body_builder::static_fragment_of(const ast::expr &ref)
    -> std::optional<result<ptr<expr>>> {
  const auto found = checked_.lowering.static_fragment_refs.find(&ref);
  if (found == checked_.lowering.static_fragment_refs.end()) {
    return std::nullopt;
  }
  if (!compile_time_) {
    return unreadable_static(ref);
  }
  auto type = type_of(ref);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  return host_call_of("static", found->second, {}, *type, ref.span);
}

auto body_builder::host_call_of(std::string operation, const void *subject,
                                ptr_vec<expr> args, type_id type,
                                source_span span) -> result<ptr<expr>> {
  if (!compile_time_) {
    return fail_feature("quote_expr",
                        "quoted syntax exists only while compiling, so run-time code "
                "cannot build or read it; use it from compile-time code (a "
                "`static def`, a `static let`, or a splice) instead",
                        span);
  }
  auto made = make_node<host_call>(span, type);
  made->operation = std::move(operation);
  made->subject = subject;
  made->args = std::move(args);
  return ptr<expr>(std::move(made));
}

auto body_builder::quote_of(const ast::quote_expr &quote) -> result<ptr<expr>> {
  if (!compile_time_) {
    return fail("quote_expr", "quoted syntax exists only while compiling",
                quote.span);
  }
  CORE_TRY(type, type_of(quote));
  // The splices the quote resolves as it is evaluated are computed first,
  // as ordinary code; the compiler then builds the syntax from them.
  auto splices = ptr_vec<expr>{};
  for (const auto *splice : comptime::fragment_builder::eager_splices(quote)) {
    const auto *operand =
        splice->kind == ast::node_kind::splice_expr
            ? ast::cast<const ast::splice_expr>(*splice).operand.get()
            : ast::cast<const ast::splice_type>(*splice).operand.get();
    if (operand == nullptr) {
      return fail("quote_expr", "a splice with nothing to splice",
                  splice->span);
    }
    CORE_TRY(value, value_of(*operand));
    splices.push_back(std::move(value));
  }
  return host_call_of("quote", &quote, std::move(splices), type, quote.span);
}

auto body_builder::fragment_builder_of(const ast::call_expr &call,
                                       std::string_view name, type_id type)
    -> result<ptr<expr>> {
  if (!compile_time_) {
    return fail_feature("quote builder",
                        "quoted syntax exists only while compiling, so run-time code "
                "cannot build or read it; use it from compile-time code (a "
                "`static def`, a `static let`, or a splice) instead",
                        call.span);
  }
  auto args = ptr_vec<expr>{};
  for (const auto &arg : call.args) {
    if (arg.value == nullptr) {
      return fail("quote builder", "an argument is missing", call.span);
    }
    CORE_TRY(value, value_of(*arg.value));
    args.push_back(std::move(value));
  }
  return host_call_of(std::format("expr.{}", name), nullptr, std::move(args),
                      type, call.span);
}

auto body_builder::function_ref_of(const semantic::resolved_callee &resolved,
                                   source_span span) -> result<ptr<expr>> {
  CORE_TRY(target, callee_of(resolved, span));
  if (target.kind != callee::form::function) {
    return fail("intrinsic function value",
                "an intrinsic named without being called", span);
  }
  auto ref = make_node<function_ref>(span, target.signature);
  ref->target = std::move(target);
  return ptr<expr>(std::move(ref));
}

auto body_builder::ident_value(const ast::ident_expr &ident)
    -> result<ptr<expr>> {
  if (checked_.ownership.generic_lambda_refs.contains(&ident)) {
    CORE_TRY(p, place_of(ident));
    return use_of(std::move(p), ident.span);
  }
  if (is_variant_ident(ident)) {
    CORE_TRY(type, type_of(ident));
    return variant_of(ident.name, type, {}, ident.span);
  }
  if (auto folded = static_value(ident)) {
    return std::move(*folded);
  }
  if (const auto found = checked_.lowering.resolved_fn_values.find(&ident);
      found != checked_.lowering.resolved_fn_values.end()) {
    return function_ref_of(found->second, ident.span);
  }
  if (lookup(ident.name).has_value()) {
    CORE_TRY(p, place_of(ident));
    return use_of(std::move(p), ident.span);
  }
  if (std::ranges::any_of(generic_params_, [&](type_id param) {
        const auto &entry = checked_.types.entry(param);
        return entry.is_value_param && entry.name == ident.name;
      })) {
    return fail("value parameter read",
                std::format("the core has no form that reads the value "
                            "parameter `{}` in a generic body",
                            ident.name),
                ident.span);
  }
  if (default_of_ != nullptr &&
      std::ranges::any_of(default_of_->params, [&](const auto &param) {
        const auto *binding =
            param.pattern == nullptr
                ? nullptr
                : ast::as<const ast::binding_pattern>(param.pattern.get());
        return binding != nullptr && binding->name == ident.name;
      })) {
    return fail_feature(
        "default argument",
        std::format("this default value refers to the parameter `{}`, but a "
                    "default is evaluated at the call site, where the call's "
                    "other arguments do not exist yet; give the parameter a "
                    "default that stands on its own (a literal, a constant, "
                    "or a call), or make it a required parameter and compute "
                    "the value in the body",
                    ident.name),
        ident.span);
  }
  return fail("missing record: resolved_fn_values",
              std::format("`{}` names no local and no recorded function",
                          ident.name),
              ident.span);
}

auto body_builder::variant_index(type_id type, std::string_view name,
                                 source_span span) -> result<std::size_t> {
  const auto *shape = find_shape(checked_, type);
  if (shape == nullptr) {
    return fail("missing record: shapes",
                std::format("no shape for `{}`", checked_.types.display(type)),
                span);
  }
  for (std::size_t i = 0; i < shape->variants.size(); ++i) {
    if (shape->variants[i].name == name) {
      return i;
    }
  }
  return fail("missing record: shapes",
              std::format("`{}` has no variant `{}`",
                          checked_.types.display(type), name),
              span);
}

auto body_builder::variant_of(const std::string &name, type_id type,
                              const std::vector<const ast::expr *> &args,
                              source_span span) -> result<ptr<expr>> {
  CORE_TRY(index, variant_index(type, name, span));
  const auto &payload = find_shape(checked_, type)->variants[index].payload;
  if (payload.size() != args.size()) {
    return fail("call_expr",
                std::format("variant `{}` takes {} payloads, given {}", name,
                            payload.size(), args.size()),
                span);
  }
  auto made = make_node<variant_init>(span, type);
  made->variant = name;
  made->variant_index = index;
  for (std::size_t i = 0; i < args.size(); ++i) {
    CORE_TRY(value, value_as(*args[i], payload[i]));
    made->payload.push_back(std::move(value));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::ordering_to_bool(ptr<expr> ordering, ast::binary_op op,
                                    source_span span) -> result<ptr<expr>> {
  const auto ordering_type = ordering->type;
  const auto less = op == ast::binary_op::lt || op == ast::binary_op::lt_eq;
  const auto equal = op == ast::binary_op::lt_eq || op == ast::binary_op::gt_eq;
  const auto greater = op == ast::binary_op::gt || op == ast::binary_op::gt_eq;
  auto made = make_node<match_expr>(span, checked_.types.bool_type());
  made->subject = temporary(std::move(ordering));
  for (const auto &[name, value] :
       {std::pair{"less", less}, std::pair{"equal", equal},
        std::pair{"greater", greater}}) {
    CORE_TRY(index, variant_index(ordering_type, name, span));
    auto arm = match_arm{};
    auto alternative = pattern_alternative{};
    alternative.test.kind = pattern_test::form::variant;
    alternative.test.span = span;
    alternative.test.subject_type = ordering_type;
    alternative.test.variant_index = index;
    arm.alternatives.push_back(std::move(alternative));
    arm.body = make_node<block>(span, checked_.types.bool_type());
    arm.body->result = bool_literal(value, span);
    arm.span = span;
    made->arms.push_back(std::move(arm));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::binary_of(const ast::binary_expr &bin) -> result<ptr<expr>> {
  if (bin.lhs == nullptr || bin.rhs == nullptr) {
    return fail_feature("binary_expr", "binary expression is missing an operand",
                        bin.span);
  }
  CORE_TRY(type, type_of(bin));
  if (ast::is_range(bin.op)) {
    return range_value(bin, type);
  }
  if (const auto found = checked_.lowering.operator_dispatches.find(&bin);
      found != checked_.lowering.operator_dispatches.end()) {
    CORE_TRY(called,
             dispatch_call(found->second, *bin.lhs, {bin.rhs.get()}, bin.span));
    if (bin.op == ast::binary_op::bang_eq) {
      auto negated = make_node<primitive>(bin.span, type);
      negated->unary = ast::unary_op::logical_not;
      negated->operands.push_back(std::move(called));
      return ptr<expr>(std::move(negated));
    }
    if (checked_.lowering.ord_dispatch_result_types.contains(&bin)) {
      return ordering_to_bool(std::move(called), bin.op, bin.span);
    }
    return called;
  }
  if (is_open_scalar(type) || is_open_operand(*bin.lhs) ||
      is_open_operand(*bin.rhs)) {
    return fail("missing record: operator_dispatches (deferred to instances)",
                std::format("`{}` on a type parameter is decided per instance",
                            ast::binary_op_name(bin.op)),
                bin.span);
  }
  switch (bin.op) {
  case ast::binary_op::pipe:
  case ast::binary_op::in:
  case ast::binary_op::not_in:
    return fail(std::format("binary_expr {}", ast::binary_op_name(bin.op)),
                "an operator with no core form yet", bin.span);
  default:
    break;
  }
  auto made = make_node<primitive>(bin.span, type);
  made->binary = bin.op;
  for (const auto *operand : {bin.lhs.get(), bin.rhs.get()}) {
    CORE_TRY(operand_type, type_of(*operand));
    CORE_TRY(value, value_as(*operand, strip_refs(operand_type)));
    made->operands.push_back(std::move(value));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::unary_of(const ast::unary_expr &un) -> result<ptr<expr>> {
  if (un.operand == nullptr) {
    return fail("unary_expr", "the operand is missing", un.span);
  }
  CORE_TRY(type, type_of(un));
  switch (un.op) {
  case ast::unary_op::addr_of:
  case ast::unary_op::addr_of_mut: {
    const auto is_mut = un.op == ast::unary_op::addr_of_mut;
    if (un.operand->kind == ast::node_kind::index_expr) {
      const auto &index = ast::cast<const ast::index_expr>(*un.operand);
      const auto &table = is_mut ? checked_.lowering.index_mut_dispatches
                                 : checked_.lowering.index_ref_dispatches;
      if (const auto found = table.find(&index); found != table.end()) {
        if (index.object == nullptr || index.index == nullptr) {
          return fail("index_expr", "an index has no object or key", un.span);
        }
        CORE_TRY(called, dispatch_call(found->second, *index.object,
                                       {index.index.get()}, un.span));
        if (is_mut && !is_ref(called->type) && is_ref(type)) {
          // A view that is not a reference (the `slice_mut[T]` of a range)
          // is a value, and `&mut` lends it, as lowering does.
          auto lent = make_node<borrow>(
              un.span, checked_.types.ref_to(called->type, true));
          lent->of = temporary(std::move(called));
          lent->is_mut = true;
          return coerce(ptr<expr>(std::move(lent)), type);
        }
        return coerce(std::move(called), type);
      }
    }
    if (checked_.types.entry(type).kind == type_kind::ptr_kind) {
      return element_address(un, type);
    }
    CORE_TRY(p, place_of(*un.operand, is_mut ? place_access::mutate
                                               : place_access::read));
    auto lent = make_node<borrow>(un.span,
                                  checked_.types.ref_to(p.type(), is_mut));
    lent->of = std::move(p);
    lent->is_mut = is_mut;
    return coerce(ptr<expr>(std::move(lent)), type);
  }
  case ast::unary_op::deref: {
    CORE_TRY(p, place_of(un));
    return use_of(std::move(p), un.span);
  }
  default: {
    CORE_TRY(operand_type, type_of(*un.operand));
    if (is_open_scalar(type) || is_open_scalar(operand_type)) {
      return fail("missing record: operator_dispatches (deferred to instances)",
                  "a unary operator on a type parameter is decided per "
                  "instance",
                  un.span);
    }
    CORE_TRY(value, value_as(*un.operand, strip_refs(operand_type)));
    auto made = make_node<primitive>(un.span, type);
    made->unary = un.op;
    made->operands.push_back(std::move(value));
    return ptr<expr>(std::move(made));
  }
  }
}

auto body_builder::element_address(const ast::unary_expr &un, type_id type)
    -> result<ptr<expr>> {
  // `&p[i]` / `&mut p[i]` on a raw pointer is the offset address
  // `p + i * size_of[T]()` (the machine layer), a raw pointer rather than a
  // borrow: an intrinsic, as lowering leaves it to the backends.
  const auto *operand = strip_groups(un.operand.get());
  if (operand == nullptr || operand->kind != ast::node_kind::index_expr) {
    return fail("unary_expr",
                std::format("`&` of a place typed as the raw pointer `{}`",
                            checked_.types.display(type)),
                un.span);
  }
  const auto &index = ast::cast<const ast::index_expr>(*operand);
  if (index.object == nullptr || index.index == nullptr) {
    return fail("index_expr", "an index has no object or key", un.span);
  }
  CORE_TRY(object_type, type_of(*index.object));
  const auto pointer = strip_refs(object_type);
  if (checked_.types.entry(pointer).kind != type_kind::ptr_kind) {
    return fail("unary_expr",
                std::format("`&` of an element of `{}` typed as `{}`",
                            checked_.types.display(pointer),
                            checked_.types.display(type)),
                un.span);
  }
  CORE_TRY(base, value_as(*index.object, pointer));
  CORE_TRY(key_type, type_of(*index.index));
  CORE_TRY(subscript, value_as(*index.index, strip_refs(key_type)));
  auto args = ptr_vec<expr>{};
  args.push_back(std::move(base));
  args.push_back(std::move(subscript));
  return intrinsic_call("ptr_offset", type, std::move(args),
                        {checked_.types.entry(type).result}, un.span);
}

auto body_builder::range_bounds(const ast::binary_expr &range,
                                bool exclusive_end)
    -> result<std::pair<ptr<expr>, ptr<expr>>> {
  if (range.lhs == nullptr || range.rhs == nullptr) {
    return fail_feature("range value", "range is missing a bound",
                        range.span);
  }
  CORE_TRY(start_type, type_of(*range.lhs));
  CORE_TRY(start, value_as(*range.lhs, strip_refs(start_type)));
  CORE_TRY(end_type, type_of(*range.rhs));
  const auto bound = strip_refs(end_type);
  CORE_TRY(end, value_as(*range.rhs, bound));
  // `a..=b` is the exclusive `a..(b + 1)`, as lowering stores it, so the
  // two bounds mean one thing wherever they are read.
  if (exclusive_end && range.op == ast::binary_op::range_inclusive) {
    auto sum = make_node<primitive>(range.rhs->span, bound);
    sum->binary = ast::binary_op::add;
    sum->operands.push_back(std::move(end));
    sum->operands.push_back(int_literal(1, bound, range.rhs->span));
    end = std::move(sum);
  }
  return std::pair{std::move(start), std::move(end)};
}

auto body_builder::range_value(const ast::binary_expr &range, type_id type)
    -> result<ptr<expr>> {
  const auto *shape = find_shape(checked_, type);
  if (shape == nullptr) {
    return fail("missing record: shapes",
                std::format("no shape for `{}`", checked_.types.display(type)),
                range.span);
  }
  CORE_TRY(bounds, range_bounds(range));
  auto made =
      make_node<aggregate_init>(node_kind::struct_init, range.span, type);
  const auto &fields = shape->fields;
  auto values = std::array{std::move(bounds.first), std::move(bounds.second)};
  const auto names = std::array<std::string_view, 2>{"start", "end"};
  for (size_t i = 0; i < names.size(); ++i) {
    const auto field =
        std::ranges::find(fields, names[i], &semantic::field_shape::name);
    if (field == fields.end()) {
      return fail("missing record: shapes",
                  std::format("`{}` has no field `{}`",
                              checked_.types.display(type), names[i]),
                  range.span);
    }
    CORE_TRY(value, coerce(std::move(values[i]), field->type));
    made->field_names.emplace_back(names[i]);
    made->field_indices.push_back(
        static_cast<size_t>(std::distance(fields.begin(), field)));
    made->elements.push_back(std::move(value));
  }
  if (made->elements.size() != fields.size()) {
    return fail("range value",
                std::format("`{}` has fields beyond `start` and `end`",
                            checked_.types.display(type)),
                range.span);
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::range_index(const ast::index_expr &index,
                               const ast::binary_expr &range, type_id type)
    -> result<ptr<expr>> {
  // Range-indexing a builtin sequence is addressing, not a call: lowering
  // hands the container and both bounds to the backends, which build the
  // view. A view the checker counts as `copy` is passed by value; any other
  // container is lent, mutably when the result is a `slice_mut`.
  CORE_TRY(p, place_of(*index.object));
  while (is_ref(p.type())) {
    add_deref(p);
  }
  const auto container = p.type();
  const auto &result_entry = checked_.types.entry(type);
  const auto is_mut = result_entry.kind == type_kind::builtin_generic_kind &&
                      result_entry.name == "slice_mut";
  auto base = ptr<expr>{};
  if (checked_.types.is_view(container) && checked_.is_copy(container)) {
    base = use_of(std::move(p), index.span);
  } else {
    base = receiver_argument(std::move(p),
                             checked_.types.ref_to(container, is_mut),
                             index.span);
  }
  // `a..=b` keeps `b` as written: the view includes it, and `b + 1` could
  // overflow.
  const auto inclusive = range.op == ast::binary_op::range_inclusive;
  CORE_TRY(bounds, range_bounds(range, !inclusive));
  auto args = ptr_vec<expr>{};
  args.push_back(std::move(base));
  args.push_back(std::move(bounds.first));
  args.push_back(std::move(bounds.second));
  return intrinsic_call(inclusive ? "range_view_inclusive" : "range_view",
                        type, std::move(args), {}, index.span);
}

auto body_builder::tuple_of(const ast::tuple_expr &tuple) -> result<ptr<expr>> {
  CORE_TRY(type, type_of(tuple));
  const auto elements = checked_.types.entry(type).args;
  if (checked_.types.entry(type).kind != type_kind::tuple_kind ||
      elements.size() != tuple.elements.size()) {
    return fail("tuple_expr", "the type is not a tuple of this length",
                tuple.span);
  }
  auto made = make_node<aggregate_init>(node_kind::tuple_init, tuple.span, type);
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (tuple.elements[i] == nullptr) {
      return fail("tuple_expr", "an element is missing", tuple.span);
    }
    CORE_TRY(value, value_as(*tuple.elements[i], elements[i]));
    made->elements.push_back(std::move(value));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::array_value(const ast::array_expr &array, type_id type)
    -> result<ptr<expr>> {
  const auto entry = checked_.types.entry(type);
  if (entry.kind != type_kind::array_kind) {
    return fail("array_expr",
                std::format("an array literal of type `{}`",
                            checked_.types.display(type)),
                array.span);
  }
  auto made = make_node<aggregate_init>(node_kind::array_init, array.span, type);
  if (array.fill_value != nullptr) {
    if (!entry.array_size.has_value()) {
      return fail("array_expr", "a fill with no constant length", array.span);
    }
    CORE_TRY(value, value_as(*array.fill_value, entry.result));
    made->elements.push_back(std::move(value));
    made->repeat = *entry.array_size;
    return ptr<expr>(std::move(made));
  }
  for (const auto &element : array.elements) {
    if (element == nullptr) {
      return fail("array_expr", "an element is missing", array.span);
    }
    CORE_TRY(value, value_as(*element, entry.result));
    made->elements.push_back(std::move(value));
  }
  return ptr<expr>(std::move(made));
}

auto body_builder::array_of(const ast::array_expr &array) -> result<ptr<expr>> {
  CORE_TRY(type, type_of(array));
  if (const auto fill = checked_.lowering.runtime_fill_dispatches.find(&array);
      fill != checked_.lowering.runtime_fill_dispatches.end()) {
    return runtime_fill_of(array, fill->second);
  }
  if (const auto found = checked_.lowering.array_literal_conversions.find(&array);
      found != checked_.lowering.array_literal_conversions.end()) {
    CORE_TRY(array_type, settle(found->second.array_type, array.span));
    CORE_TRY(inner, array_value(array, array_type));
    CORE_TRY(target, callee_of(found->second.callee, array.span));
    const auto params = checked_.types.entry(target.signature).args;
    if (params.size() != 1) {
      return fail("missing record: array_literal_conversions",
                  "the conversion does not take one parameter", array.span);
    }
    auto args = ptr_vec<expr>{};
    CORE_TRY(converted, coerce(std::move(inner), params[0]));
    args.push_back(std::move(converted));
    return direct_call(std::move(target), std::move(args), array.span);
  }
  return array_value(array, type);
}

auto body_builder::struct_of(const ast::struct_expr &literal)
    -> result<ptr<expr>> {
  CORE_TRY(type, type_of(literal));
  const auto *shape = find_shape(checked_, type);
  if (shape == nullptr) {
    return fail("missing record: shapes",
                std::format("no shape for `{}`", checked_.types.display(type)),
                literal.span);
  }
  const auto fields = shape->fields;
  auto made =
      make_node<aggregate_init>(node_kind::struct_init, literal.span, type);
  // Source order: each initializer is evaluated where it is written; its
  // position in the declaration is recorded beside it.
  for (const auto &init : literal.fields) {
    const auto field = std::ranges::find(fields, init.name,
                                         &semantic::field_shape::name);
    if (field == fields.end()) {
      return fail("missing record: shapes",
                  std::format("`{}` has no field `{}`",
                              checked_.types.display(type), init.name),
                  init.span);
    }
    if (init.value != nullptr) {
      CORE_TRY(value, value_as(*init.value, field->type));
      made->elements.push_back(std::move(value));
    } else {
      auto name = init.name;
      if (const auto copy = checked_.ownership.generic_lambda_refs.find(&init);
          copy != checked_.ownership.generic_lambda_refs.end()) {
        name = copy_name(init.name, *copy->second);
      }
      const auto local = lookup(name);
      if (!local.has_value()) {
        return fail("struct_expr",
                    std::format("shorthand `{}` names no local", init.name),
                    init.span);
      }
      auto p = place{};
      p.root.kind = place_root::origin::local;
      p.root.symbol = local->symbol;
      p.root.name = init.name;
      p.root.type = local->type;
      CORE_TRY(element, coerce(use_of(std::move(p), init.span), field->type));
      made->elements.push_back(std::move(element));
    }
    made->field_names.push_back(init.name);
    made->field_indices.push_back(
        static_cast<std::size_t>(field - fields.begin()));
  }
  if (made->elements.size() != fields.size()) {
    return fail("struct_expr", "a literal that does not name every field once",
                literal.span);
  }
  return checked_construction(ptr<expr>(std::move(made)));
}

auto body_builder::where_of(const ast::where_expr &where) -> result<ptr<expr>> {
  CORE_TRY(type, type_of(where));
  if (where.inner == nullptr) {
    return fail("where_expr", "no inner expression", where.span);
  }
  push_scope();
  auto made = make_node<block>(where.span, type);
  for (const auto &binding : where.bindings) {
    if (binding.value == nullptr) {
      pop_scope();
      return fail("where_expr", "a binding has no value", binding.span);
    }
    auto value = value_of(*binding.value);
    if (!value.has_value()) {
      pop_scope();
      return std::unexpected(value.error());
    }
    auto bound = make_node<let>(binding.span);
    bound->type = (*value)->type;
    bound->name = binding.name;
    bound->value = std::move(*value);
    bound->symbol = declare(binding.name, bound->type);
    // The drop schedule names a `where` binding by the binding.
    bound->binding_origin = &binding;
    made->statements.push_back(std::move(bound));
  }
  auto inner = value_as(*where.inner, type);
  pop_scope();
  if (!inner.has_value()) {
    return std::unexpected(inner.error());
  }
  made->result = std::move(*inner);
  return ptr<expr>(std::move(made));
}

auto body_builder::value_of(const ast::expr &source) -> result<ptr<expr>> {
  auto made = value_of_node(source);
  if (made.has_value() && (*made)->origin == nullptr) {
    (*made)->origin = &source;
  }
  return made;
}

auto body_builder::value_of_node(const ast::expr &source)
    -> result<ptr<expr>> {
  if (const auto rewritten = checked_.spliced_fragments.find(&source);
      rewritten != checked_.spliced_fragments.end()) {
    if (const auto *fragment = ast::as<const ast::expr>(rewritten->second)) {
      return value_of(*fragment);
    }
  }
  if (source.has_error) {
    return fail("error node", "the expression carries a parse error",
                source.span);
  }
  if (const auto *folded = static_fold(source)) {
    return value_of(*folded);
  }
  switch (source.kind) {
  case ast::node_kind::group_expr: {
    const auto &group = ast::cast<const ast::group_expr>(source);
    if (group.inner == nullptr) {
      return fail("group_expr", "empty parentheses", source.span);
    }
    return value_of(*group.inner);
  }
  case ast::node_kind::literal_expr:
    return literal_of(ast::cast<const ast::literal_expr>(source));
  case ast::node_kind::ident_expr:
    return ident_value(ast::cast<const ast::ident_expr>(source));
  case ast::node_kind::binary_expr:
    return binary_of(ast::cast<const ast::binary_expr>(source));
  case ast::node_kind::unary_expr:
    return unary_of(ast::cast<const ast::unary_expr>(source));
  case ast::node_kind::lambda_expr:
    return lambda_of(ast::cast<const ast::lambda_expr>(source));
  case ast::node_kind::interpolated_string_expr:
    return interpolation_of(
        ast::cast<const ast::interpolated_string_expr>(source));
  case ast::node_kind::await_expr:
    return await_of(ast::cast<const ast::await_expr>(source));
  case ast::node_kind::async_expr: {
    CORE_TRY(type, type_of(source));
    CORE_TRY(declared, task_result_type(type, source.span));
    return task_block_of(&source, ast::cast<const ast::async_expr>(source).body,
                         declared, source.span);
  }
  case ast::node_kind::on_expr: {
    CORE_TRY(type, type_of(source));
    auto declared = task_result_type(type, source.span);
    if (!declared.has_value()) {
      return fail_feature("on_expr",
                          std::format("an `on(...)` did not resolve to a task "
                                      "type (it is typed `{}`)",
                                      checked_.types.display(type)),
                          source.span);
    }
    return on_task_of(ast::cast<const ast::on_expr>(source), *declared, type);
  }
  case ast::node_kind::crew_expr: {
    const auto &crew = ast::cast<const ast::crew_expr>(source);
    CORE_TRY(type, type_of(source));
    return crew_of(&source, crew.name, crew.body, source.span, type);
  }
  case ast::node_kind::call_expr:
    return call_of(ast::cast<const ast::call_expr>(source));
  case ast::node_kind::field_expr: {
    if (auto folded = static_value(source)) {
      return std::move(*folded);
    }
    CORE_TRY(p, place_of(source));
    return use_of(std::move(p), source.span);
  }
  case ast::node_kind::module_path_expr: {
    if (auto folded = static_value(source)) {
      return std::move(*folded);
    }
    if (const auto found = checked_.lowering.resolved_fn_values.find(&source);
        found != checked_.lowering.resolved_fn_values.end()) {
      return function_ref_of(found->second, source.span);
    }
    CORE_TRY(p, place_of(source));
    return use_of(std::move(p), source.span);
  }
  case ast::node_kind::index_expr: {
    const auto &index = ast::cast<const ast::index_expr>(source);
    if (const auto found = checked_.lowering.index_dispatches.find(&index);
        found != checked_.lowering.index_dispatches.end() &&
        index.object != nullptr && index.index != nullptr) {
      CORE_TRY(type, type_of(source));
      if (!(checked_.lowering.index_ref_dispatches.contains(&index) &&
            !checked_.is_copy(type))) {
        CORE_TRY(called, dispatch_call(found->second, *index.object,
                                       {index.index.get()}, source.span));
        return coerce(std::move(called), type);
      }
    }
    CORE_TRY(p, place_of(source));
    return use_of(std::move(p), source.span);
  }
  case ast::node_kind::block_expr: {
    const auto &block_ast = ast::cast<const ast::block_expr>(source);
    CORE_TRY(type, type_of(source));
    CORE_TRY(made, block_of(block_ast.stmts, source.span, type));
    return ptr<core::expr>(std::move(made));
  }
  case ast::node_kind::if_expr: {
    const auto &if_ast = ast::cast<const ast::if_expr>(source);
    if (if_ast.branches.empty()) {
      return fail("if_expr", "no branches", source.span);
    }
    CORE_TRY(type, type_of(source));
    return if_of(if_ast.branches, if_ast.else_body, 0, source.span, type);
  }
  case ast::node_kind::match_expr: {
    const auto &match_ast = ast::cast<const ast::match_expr>(source);
    if (match_ast.subject == nullptr) {
      return fail("match_expr", "no subject", source.span);
    }
    CORE_TRY(type, type_of(source));
    return match_of(*match_ast.subject, match_ast.arms, source.span, type);
  }
  case ast::node_kind::tuple_expr:
    return tuple_of(ast::cast<const ast::tuple_expr>(source));
  case ast::node_kind::array_expr:
    return array_of(ast::cast<const ast::array_expr>(source));
  case ast::node_kind::struct_expr:
    return struct_of(ast::cast<const ast::struct_expr>(source));
  case ast::node_kind::cast_expr: {
    const auto &cast_ast = ast::cast<const ast::cast_expr>(source);
    if (cast_ast.operand == nullptr) {
      return fail("cast_expr", "no operand", source.span);
    }
    CORE_TRY(type, type_of(source));
    CORE_TRY(operand_type, type_of(*cast_ast.operand));
    if (is_open_scalar(type) || is_open_scalar(operand_type)) {
      return fail("open cast (deferred to instances)",
                  std::format("a cast from `{}` to `{}` is decided per "
                              "instance",
                              checked_.types.display(operand_type),
                              checked_.types.display(type)),
                  source.span);
    }
    CORE_TRY(operand, value_as(*cast_ast.operand, strip_refs(operand_type)));
    auto made = make_node<cast>(source.span, type);
    made->operand = std::move(operand);
    return ptr<core::expr>(std::move(made));
  }
  case ast::node_kind::where_expr:
    return where_of(ast::cast<const ast::where_expr>(source));
  case ast::node_kind::for_expr:
    return comprehension_of(ast::cast<const ast::for_expr>(source));
  case ast::node_kind::try_expr:
    return try_of(ast::cast<const ast::try_expr>(source));
  case ast::node_kind::or_else_expr:
    return or_else_of(ast::cast<const ast::or_else_expr>(source));
  case ast::node_kind::quote_expr:
    return quote_of(ast::cast<const ast::quote_expr>(source));
  default:
    return fail_feature(kind_name(source.kind),
                        std::format("expression kind {} is not lowered yet "
                                    "(see spec/todo.md)",
                                    static_cast<int>(source.kind)),
                        source.span);
  }
}

// ==========================================================================
//  Calls
// ==========================================================================

auto body_builder::callee_of(const semantic::resolved_callee &resolved,
                             source_span span) -> result<callee> {
  if (resolved.decl == nullptr) {
    return fail("missing record: resolved_callees", "a callee with no decl",
                span);
  }
  if (resolved.bound.has_value()) {
    return trait_method_callee(resolved, span);
  }
  // Inside a template, a callee that is itself a template (the template
  // itself included) is called at the arguments the checker recorded, in
  // this body's parameters (`template_args`), and its signature is restated
  // under them. A call it recorded none for is solved per instance only.
  const std::vector<type_id> *template_params = nullptr;
  if (templates_ != nullptr && !generic_params_.empty()) {
    if (const auto found = templates_->find(resolved.decl);
        found != templates_->end()) {
      if (resolved.template_args.size() != found->second.size()) {
        return fail("missing record: resolved_callees (deferred to instances)",
                    std::format("a generic body calls the template `{}` with "
                                "no recorded arguments; only its instances "
                                "record them",
                                resolved.decl->name),
                    span);
      }
      template_params = &found->second;
    }
  }
  // A template callee's signature is read in its own parameters, which are
  // open there and not in this body.
  auto saved_open = std::optional<std::unordered_set<type_id>>{};
  if (template_params != nullptr) {
    saved_open = std::exchange(
        open_params_, std::unordered_set<type_id>(template_params->begin(),
                                                  template_params->end()));
  }
  auto sig_result = signature_of(*resolved.decl);
  if (saved_open.has_value()) {
    open_params_ = std::move(*saved_open);
  }
  if (!sig_result.has_value()) {
    return std::unexpected(std::move(sig_result.error()));
  }
  const auto &sig = *sig_result;
  auto target = callee{};
  target.signature = sig.fn_type;
  if (template_params != nullptr) {
    target.generic_args = resolved.template_args;
    target.signature = template_signature(checked_.types, sig.fn_type,
                                          *template_params,
                                          resolved.template_args);
  }
  target.receiver = sig.receiver;
  target.owner_module = resolved.owner_module;
  target.impl_target = resolved.impl_target_type;
  target.trait_name = resolved.trait_name;
  target.name = resolved.impl_target_type.empty()
                    ? resolved.decl->name
                    : std::format("{}::{}", resolved.impl_target_type,
                                  resolved.decl->name);
  if (resolved.decl->modifiers.is_intrinsic) {
    target.kind = callee::form::intrinsic;
    return target;
  }
  target.kind = callee::form::function;
  target.decl = resolved.decl;
  if (const auto tmpl = checked_.ownership.instance_templates.find(resolved.decl);
      tmpl != checked_.ownership.instance_templates.end()) {
    target.instance_of = tmpl->second;
    if (const auto args = checked_.ownership.instance_generic_args.find(resolved.decl);
        args != checked_.ownership.instance_generic_args.end()) {
      target.generic_args = args->second;
    }
  }
  return target;
}

auto body_builder::trait_method_callee(const semantic::resolved_callee &resolved,
                                       source_span span) -> result<callee> {
  const auto &bound = *resolved.bound;
  const auto entry = checked_.trait_method_signatures.find(resolved.decl);
  if (entry == checked_.trait_method_signatures.end()) {
    return fail("missing record: trait_method_signatures",
                std::format("the trait method `{}` has no declared signature",
                            resolved.decl->name),
                span);
  }
  const auto &declared = entry->second;
  // The trait's signature, with `Self`, the trait's parameters and the
  // method's own replaced by the bound's subject, its arguments and the
  // call's. A count that does not match is left for the verifier to
  // report (C9).
  auto args = std::vector<type_id>{bound.self_type};
  args.insert(args.end(), bound.trait_args.begin(), bound.trait_args.end());
  args.insert(args.end(), resolved.template_args.begin(),
              resolved.template_args.end());
  auto target = callee{};
  target.kind = callee::form::trait_method;
  target.decl = resolved.decl;
  target.name = resolved.decl->name;
  target.owner_module = declared.owner_module;
  target.trait_name = declared.trait_name;
  target.generic_args = resolved.template_args;
  target.self_type = bound.self_type;
  target.trait_args = bound.trait_args;
  target.signature = template_signature(checked_.types, declared.signature,
                                        declared.generic_params, args);
  switch (declared.receiver) {
  case semantic::trait_receiver::none:
    target.receiver = receiver_mode::none;
    break;
  case semantic::trait_receiver::shared:
    target.receiver = receiver_mode::shared;
    break;
  case semantic::trait_receiver::exclusive:
    target.receiver = receiver_mode::exclusive;
    break;
  case semantic::trait_receiver::consuming:
    target.receiver = receiver_mode::consuming;
    break;
  }
  return target;
}

auto body_builder::direct_call(callee target, ptr_vec<expr> args,
                               source_span span) -> ptr<expr> {
  const auto result_type = checked_.types.entry(target.signature).result;
  auto made = make_node<call>(span, result_type);
  made->target = std::move(target);
  made->args = std::move(args);
  return made;
}

auto body_builder::intrinsic_call(std::string name, type_id type,
                                  ptr_vec<expr> args,
                                  std::vector<type_id> generic_args,
                                  source_span span) -> ptr<expr> {
  auto params = std::vector<type_id>{};
  for (const auto &arg : args) {
    params.push_back(arg->type);
  }
  auto target = callee{};
  target.kind = callee::form::intrinsic;
  target.name = std::move(name);
  target.generic_args = std::move(generic_args);
  target.signature = checked_.types.fn_of(std::move(params), type);
  return direct_call(std::move(target), std::move(args), span);
}

auto body_builder::receiver_argument(place p, type_id param, source_span span)
    -> ptr<expr> {
  const auto &param_entry = checked_.types.entry(param);
  if (param_entry.kind == type_kind::ref_kind) {
    const auto inner = param_entry.result;
    const auto is_mut = param_entry.is_mut;
    auto added = false;
    while (p.type() != inner && is_ref(p.type())) {
      add_deref(p);
      added = true;
    }
    if (p.type() == inner) {
      auto lent = make_node<borrow>(span, param);
      lent->of = std::move(p);
      lent->is_mut = is_mut;
      lent->inserted = true;
      lent->two_phase = is_mut && !added;
      return lent;
    }
    return use_of(std::move(p), span);
  }
  while (p.type() != param && is_ref(p.type())) {
    add_deref(p);
  }
  return use_of(std::move(p), span);
}

auto body_builder::argument(const ast::expr &arg, type_id param,
                            std::optional<semantic::param_passing> passing)
    -> result<ptr<expr>> {
  // An inserted borrow of the argument's place is the one node made from
  // it; anything else came from `value_of`, which named it already.
  auto made = argument_of(arg, param, passing);
  if (made.has_value() && (*made)->origin == nullptr &&
      (*made)->kind == node_kind::borrow &&
      static_cast<const borrow &>(**made).inserted) {
    (*made)->origin = &arg;
  }
  return made;
}

auto body_builder::argument_of(const ast::expr &arg, type_id param,
                               std::optional<semantic::param_passing> passing)
    -> result<ptr<expr>> {
  const auto &param_entry = checked_.types.entry(param);
  const auto mode =
      passing.value_or(param_entry.kind != type_kind::ref_kind
                           ? semantic::param_passing::by_value
                       : param_entry.is_mut ? semantic::param_passing::mut_ref
                                            : semantic::param_passing::shared_ref);
  if (mode == semantic::param_passing::by_value) {
    return value_as(arg, param);
  }
  // A written `&e` / `&mut e`, or an argument already a reference, is passed
  // as it is (a shared reference is copied; `&mut` to `&` reborrows).
  const auto *bare = strip_groups(&arg);
  if (bare != nullptr && bare->kind == ast::node_kind::unary_expr) {
    const auto op = ast::cast<const ast::unary_expr>(*bare).op;
    if (op == ast::unary_op::addr_of || op == ast::unary_op::addr_of_mut) {
      return value_as(arg, param);
    }
  }
  CORE_TRY(p, place_of(arg, mode == semantic::param_passing::mut_ref
                                ? place_access::mutate
                                : place_access::read));
  if (is_ref(p.type()) && mode == semantic::param_passing::shared_ref) {
    return coerce(use_of(std::move(p), arg.span), param);
  }
  return receiver_argument(std::move(p), param, arg.span);
}

auto body_builder::dispatch_call(const semantic::resolved_callee &resolved,
                                 const ast::expr &receiver,
                                 std::vector<const ast::expr *> rest,
                                 source_span span) -> result<ptr<expr>> {
  CORE_TRY(target, callee_of(resolved, span));
  const auto params = checked_.types.entry(target.signature).args;
  if (params.size() != 1 + rest.size()) {
    return fail("missing record: operator_dispatches",
                std::format("`{}` takes {} parameters, the dispatch passes {}",
                            target.name, params.size(), 1 + rest.size()),
                span);
  }
  auto args = ptr_vec<expr>{};
  CORE_TRY(first, argument(receiver, params[0], std::nullopt));
  args.push_back(std::move(first));
  for (std::size_t i = 0; i < rest.size(); ++i) {
    CORE_TRY(next, argument(*rest[i], params[i + 1], std::nullopt));
    args.push_back(std::move(next));
  }
  return direct_call(std::move(target), std::move(args), span);
}

auto body_builder::builtin_method(const ast::call_expr &call,
                                  const ast::field_expr &field)
    -> std::optional<result<ptr<expr>>> {
  if (field.object == nullptr) {
    return std::nullopt;
  }
  const auto &name = field.field_name;
  if (name != "len" && name != "as_ptr" && name != "as_mut_ptr" &&
      name != "as_bytes" && name != "spawn" && name != "errors" &&
      name != "get" && name != "next" && name != "set") {
    return std::nullopt;
  }
  auto type = type_of(call);
  if (!type.has_value()) {
    return std::unexpected(type.error());
  }
  const auto object_type = checked_.node_types.find(field.object.get());
  if (object_type == checked_.node_types.end()) {
    return fail("missing record: node_types", "a builtin method's receiver",
                field.span);
  }
  const auto container = strip_refs(object_type->second);
  const auto &entry = checked_.types.entry(container);
  const auto generic_named = [&](std::string_view generic) {
    return entry.kind == type_kind::builtin_generic_kind &&
           entry.name == generic;
  };
  // The receiver, as a place with its reference layers followed.
  const auto receiver = [&](place_access access =
                                place_access::read) -> result<place> {
    CORE_TRY(p, receiver_place(*field.object, access));
    while (is_ref(p.type())) {
      add_deref(p);
    }
    return p;
  };
  const auto by_borrow = [&](bool is_mut) -> result<ptr<expr>> {
    CORE_TRY(p, receiver(is_mut ? place_access::mutate : place_access::read));
    const auto param = checked_.types.ref_to(p.type(), is_mut);
    return receiver_argument(std::move(p), param, field.span);
  };
  const auto by_value = [&]() -> result<ptr<expr>> {
    CORE_TRY(p, receiver());
    return use_of(std::move(p), field.span);
  };
  const auto one = [&](result<ptr<expr>> arg, std::string op)
      -> result<ptr<expr>> {
    if (!arg.has_value()) {
      return std::unexpected(arg.error());
    }
    auto args = ptr_vec<expr>{};
    args.push_back(std::move(*arg));
    return intrinsic_call(std::move(op), *type, std::move(args), {}, call.span);
  };
  if (auto task = task_method(call, field, *type)) {
    return std::move(*task);
  }
  if (name == "len") {
    if (entry.kind == type_kind::array_kind && entry.array_size.has_value()) {
      auto lit = make_node<literal>(call.span, *type);
      lit->token = token_kind::int_lit;
      lit->spelling = std::to_string(*entry.array_size);
      return ptr<expr>(std::move(lit));
    }
    if (generic_named("uninit")) {
      // The number of elements is a layout fact (how many slots the buffer
      // holds), which lowering folds to a constant; the core has no layout.
      return intrinsic_call("uninit_len", *type, {}, {container}, call.span);
    }
    return one(checked_.types.is_view(container) && checked_.is_copy(container) ? by_value() : by_borrow(false),
               "len");
  }
  if (name == "as_ptr" || name == "as_mut_ptr") {
    return one(checked_.types.is_view(container) &&
                       checked_.is_copy(container)
                   ? by_value()
                   : by_borrow(name == "as_mut_ptr"),
               "container_data");
  }
  if (name == "as_bytes") {
    return one(by_value(), "as_bytes");
  }
  if (name == "next" && generic_named("generator")) {
    return one(by_borrow(true), "generator_next");
  }
  return std::nullopt;
}

auto body_builder::function_value(const ast::expr &source, type_id fn_type)
    -> result<ptr<expr>> {
  // Calling a function value does not consume it (C4). A temporary is
  // passed as it is; a place is lent: a copy of it when it is already a
  // `&fn`, else a borrow of the `fn` behind its reference layers.
  CORE_TRY(p, place_of(source));
  const auto shared = checked_.types.ref_to(fn_type, false);
  if (p.root.kind == place_root::origin::temporary && p.path.empty() &&
      (p.type() == fn_type || p.type() == shared)) {
    return std::move(p.root.value);
  }
  if (p.type() == shared) {
    return use_of(std::move(p), source.span);
  }
  while (p.type() != fn_type && is_ref(p.type())) {
    add_deref(p);
  }
  if (p.type() != fn_type) {
    return fail("implicit conversion",
                std::format("calling a `{}` as `{}`",
                            checked_.types.display(p.type()),
                            checked_.types.display(fn_type)),
                source.span);
  }
  auto lent = make_node<borrow>(source.span, shared);
  lent->of = std::move(p);
  lent->inserted = true;
  return ptr<expr>(std::move(lent));
}

auto body_builder::call_of(const ast::call_expr &call) -> result<ptr<expr>> {
  if (call.callee == nullptr) {
    return fail("call_expr", "no callee", call.span);
  }
  CORE_TRY(type, type_of(call));
  const auto &lowering = checked_.lowering;
  if (const auto reflected = lowering.type_param_reflections.find(&call);
      reflected != lowering.type_param_reflections.end()) {
    auto lit = make_node<literal>(call.span, type);
    lit->token = token_kind::string_lit;
    lit->spelling = encode_string_literal(reflected->second.type_name);
    return ptr<expr>(std::move(lit));
  }
  if (const auto folded = lowering.reflection_folds.find(&call);
      folded != lowering.reflection_folds.end()) {
    return reified(folded->second, call.span);
  }
  if (const auto folded = lowering.folded_comptime_calls.find(&call);
      folded != lowering.folded_comptime_calls.end()) {
    return literal_of(*folded->second);
  }
  // A `static def` call in a block `static for`, folded in each iteration.
  if (const auto *folded = static_fold(call)) {
    return value_of(*folded);
  }
  if (lowering.stack_buffers.contains(&call)) {
    return intrinsic_call("stack_buffer", type, {}, {type}, call.span);
  }
  if (const auto cast_to = lowering.ptr_casts.find(&call);
      cast_to != lowering.ptr_casts.end()) {
    if (call.args.size() != 1 || call.args[0].value == nullptr) {
      return fail("call_expr", "`ptr_cast` has no operand", call.span);
    }
    CORE_TRY(target, settle(cast_to->second, call.span));
    // The record holds the pointer type the cast makes; the operation's
    // type operand is what it points at.
    const auto &target_entry = checked_.types.entry(target);
    if (target_entry.kind != type_kind::ptr_kind) {
      return fail("missing record: ptr_casts",
                  std::format("the cast makes a `{}`, not a raw pointer",
                              checked_.types.display(target)),
                  call.span);
    }
    const auto pointee = target_entry.result;
    CORE_TRY(operand, value_of(*call.args[0].value));
    auto args = ptr_vec<expr>{};
    args.push_back(std::move(operand));
    return intrinsic_call("ptr_cast", type, std::move(args), {pointee},
                          call.span);
  }
  if (lowering.slice_from_raw_parts_calls.contains(&call)) {
    if (call.args.size() != 2 || call.args[0].value == nullptr ||
        call.args[1].value == nullptr) {
      return fail("call_expr", "`slice_from_raw_parts` needs two operands",
                  call.span);
    }
    auto args = ptr_vec<expr>{};
    for (const auto &arg : call.args) {
      CORE_TRY(value, value_of(*arg.value));
      args.push_back(std::move(value));
    }
    // The checker types a shared `slice[T]` from a `*mut T` too; the
    // pointer is then read as the `*T` that view needs.
    const auto &result_entry = checked_.types.entry(type);
    const auto &pointer = checked_.types.entry(args[0]->type);
    if (result_entry.kind == type_kind::builtin_generic_kind &&
        result_entry.name == "slice" && pointer.kind == type_kind::ptr_kind &&
        pointer.is_mut) {
      CORE_TRY(shared, coerce(std::move(args[0]),
                              checked_.types.ptr_to(pointer.result, false)));
      args[0] = std::move(shared);
    }
    return intrinsic_call("slice_from_raw_parts", type, std::move(args), {},
                          call.span);
  }
  if (const auto query = lowering.layout_queries.find(&call);
      query != lowering.layout_queries.end()) {
    if (query->second.operand == k_unknown_type ||
        query->second.operand == k_error_type) {
      return fail_feature("unsettled type",
                          "cannot tell which type this asks the size or "
                          "alignment of — name a concrete type, or a type "
                          "parameter of the enclosing function",
                          call.span);
    }
    CORE_TRY(operand, settle(query->second.operand, call.span));
    const auto *name =
        query->second.kind == semantic::layout_query_kind::size_of ? "size_of"
        : query->second.kind == semantic::layout_query_kind::align_of
            ? "align_of"
            : "needs_drop";
    return intrinsic_call(name, type, {}, {operand}, call.span);
  }
  const auto resolved = checked_.resolved_callees.find(&call);
  if (call.callee->kind == ast::node_kind::field_expr &&
      resolved == checked_.resolved_callees.end()) {
    const auto &field = ast::cast<const ast::field_expr>(*call.callee);
    if (const auto *object = ast::as<const ast::ident_expr>(field.object.get());
        object != nullptr && object->name == "expr") {
      if (comptime::fragment_builder::is_builder(field.field_name)) {
        return fragment_builder_of(call, field.field_name, type);
      }
      return fail_feature("quote builder",
                        "quoted syntax exists only while compiling, so run-time code "
                "cannot build or read it; use it from compile-time code (a "
                "`static def`, a `static let`, or a splice) instead",
                        call.span);
    }
    if (auto built = builtin_method(call, field)) {
      return std::move(*built);
    }
    if (!generic_params_.empty()) {
      // A method reached through a bound, or on a type that mentions a
      // parameter: the template leaves which method it is to the instances.
      return fail("missing record: resolved_callees (deferred to instances)",
                  std::format("the method `{}` is resolved per instance",
                              field.field_name),
                  call.span);
    }
  }
  if (call.callee->kind == ast::node_kind::ident_expr) {
    const auto &ident = ast::cast<const ast::ident_expr>(*call.callee);
    if (is_variant_ident(ident)) {
      auto args = std::vector<const ast::expr *>{};
      for (const auto &arg : call.args) {
        if (arg.value == nullptr || arg.name.has_value()) {
          return fail_feature("call_expr",
                          "named arguments are not supported constructing a "
                          "sum-type variant",
                      arg.span);
        }
        args.push_back(arg.value.get());
      }
      return variant_of(ident.name, type, args, call.span);
    }
    if (semantic::is_builtin_scalar_name(ident.name) && call.args.size() == 1 &&
        call.args.front().value != nullptr &&
        !call.args.front().name.has_value() &&
        !checked_.node_types.contains(call.callee.get())) {
      const auto &operand_ast = *call.args.front().value;
      CORE_TRY(operand_type, type_of(operand_ast));
      CORE_TRY(operand, value_as(operand_ast, strip_refs(operand_type)));
      auto made = make_node<cast>(call.span, type);
      made->operand = std::move(operand);
      return ptr<expr>(std::move(made));
    }
  }

  // The callee, and the receiver when it is a method call.
  auto target = callee{};
  const ast::expr *receiver_ast = nullptr;
  if (resolved != checked_.resolved_callees.end()) {
    CORE_TRY(found, callee_of(resolved->second, call.span));
    target = std::move(found);
    receiver_ast = resolved->second.receiver;
  } else if (const auto named =
                 checked_.lowering.resolved_fn_values.find(call.callee.get());
             named != checked_.lowering.resolved_fn_values.end() &&
             (call.callee->kind == ast::node_kind::ident_expr ||
              call.callee->kind == ast::node_kind::module_path_expr) &&
             !checked_.ownership.generic_lambda_refs.contains(
                 call.callee.get())) {
    CORE_TRY(found, callee_of(named->second, call.span));
    target = std::move(found);
  } else {
    const auto callee_type = checked_.node_types.find(call.callee.get());
    if (callee_type == checked_.node_types.end() ||
        checked_.types.entry(strip_refs(callee_type->second)).kind !=
            type_kind::fn_kind) {
      return fail("missing record: resolved_callees",
                  "a call with no resolved callee and no `fn` type", call.span);
    }
    if (const auto *ident = ast::as<const ast::ident_expr>(call.callee.get());
        ident != nullptr && !lookup(ident->name).has_value() &&
        !checked_.ownership.generic_lambda_refs.contains(ident)) {
      return fail("missing record: resolved_callees",
                  std::format("bare call of `{}`, which names no local and "
                              "has no callee record",
                              ident->name),
                  call.span);
    }
    CORE_TRY(fn_type, settle(strip_refs(callee_type->second), call.span));
    CORE_TRY(fn_value, function_value(*call.callee, fn_type));
    target.kind = callee::form::value;
    target.signature = fn_type;
    target.value = std::move(fn_value);
  }

  const auto params = checked_.types.entry(target.signature).args;
  auto args = ptr_vec<expr>{};
  auto offset = std::size_t{0};
  if (receiver_ast != nullptr) {
    if (params.empty()) {
      return fail("missing record: resolved_callees",
                  "a method call to a callee with no parameters", call.span);
    }
    const auto &first = checked_.types.entry(params[0]);
    CORE_TRY(p, receiver_place(*receiver_ast,
                               first.kind == type_kind::ref_kind && first.is_mut
                                   ? place_access::mutate
                                   : place_access::read));
    args.push_back(receiver_argument(std::move(p), params[0], receiver_ast->span));
    args.back()->origin = receiver_ast;
    offset = 1;
  }
  if (const auto mapping = checked_.call_argument_mappings.find(&call);
      mapping != checked_.call_argument_mappings.end()) {
    const auto &map = mapping->second;
    if (map.args_by_param.size() + offset != params.size()) {
      return fail("missing record: call_argument_mappings",
                  std::format("{} mapped arguments for {} parameters",
                              map.args_by_param.size() + offset, params.size()),
                  call.span);
    }
    for (std::size_t i = 0; i < map.args_by_param.size(); ++i) {
      const auto *arg = map.args_by_param[i] != nullptr
                            ? map.args_by_param[i]
                        : i < map.defaults_by_param.size()
                            ? map.defaults_by_param[i]
                            : nullptr;
      if (arg == nullptr) {
        return fail("missing record: call_argument_mappings",
                    "an omitted argument with no default", call.span);
      }
      const auto passing =
          i < map.passing_by_param.size()
              ? std::optional{map.passing_by_param[i]}
              : std::nullopt;
      const auto *outer_default = default_of_;
      if (map.args_by_param[i] == nullptr) {
        default_of_ = target.decl;
      }
      auto value = argument(*arg, params[offset + i], passing);
      default_of_ = outer_default;
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      args.push_back(std::move(*value));
    }
  } else {
    if (call.args.size() + offset != params.size()) {
      return fail("missing record: call_argument_mappings",
                  std::format("{} arguments for {} parameters",
                              call.args.size() + offset, params.size()),
                  call.span);
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      if (call.args[i].value == nullptr || call.args[i].name.has_value()) {
        return fail_feature("call_expr",
                            "named arguments are only supported calling a "
                            "named function or method, not a bare function "
                            "value",
                    call.args[i].span);
      }
      CORE_TRY(value,
               argument(*call.args[i].value, params[offset + i], std::nullopt));
      args.push_back(std::move(value));
    }
  }
  return direct_call(std::move(target), std::move(args), call.span);
}

} // namespace cinder::core
