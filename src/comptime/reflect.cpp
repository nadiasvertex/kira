#include <algorithm>
#include <format>

#include "src/comptime/eval.h"

namespace kira::comptime {

namespace {

/// Renders a syntactic `type_expr` back to a display string, purely from
/// its own AST shape — no `type_id`/`type_table` involved, since the
/// evaluator has no type-checker access (see `reflect.cpp`'s design plan
/// section 4 note: reflection here is a read-only traversal of already-
/// parsed declaration syntax). Deliberately narrow, matching every other
/// M6 gap: only `named_type` (the overwhelmingly common case for a struct
/// field's declared type) renders precisely; anything else — a reference,
/// pointer, tuple, array, function type, ... — renders as the placeholder
/// `"<type>"` rather than guessing at a possibly-misleading spelling.
[[nodiscard]] auto render_type_expr(const ast::type_expr *type) -> std::string {
  if (type == nullptr) {
    return "<type>";
  }
  if (type->kind != ast::node_kind::named_type) {
    return "<type>";
  }
  const auto &named = dynamic_cast<const ast::named_type &>(*type);
  auto rendered = std::string{};
  for (size_t i = 0; i < named.path.size(); ++i) {
    if (i > 0) {
      rendered += '.';
    }
    rendered += named.path[i];
  }
  if (!named.type_args.empty()) {
    rendered += "[..]";
  }
  return rendered;
}

/// Builds a `struct_instance` value describing one field: `{name: "x",
/// type_name: "int32", type_of: <type_expr>, is_data_member: true}` — the
/// shape `static for field in T.fields():` code is expected to destructure.
/// Named `type_name`, not `type`: `type` is a reserved keyword in Kira, so a
/// quoted `field.type` access could never parse. `type_of` carries the
/// field's declared `type_expr` fragment directly (splice-ready with `~`),
/// resolving `spec/todo.md` item 5's precision loss in `type_name` (generic
/// arguments collapse to `"[..]"`, non-`named_type` shapes to `"<type>"`);
/// `type_name` stays for existing consumers (`src/std/deriving.kira`) that
/// only ever read it for display, never for type-dependent dispatch.
/// `is_data_member` is always `true` here — see `make_module_member_
/// descriptor`'s doc comment for the `false` case.
[[nodiscard]] auto make_field_descriptor(const ast::struct_field &field)
    -> value {
  auto fields = std::unordered_map<std::string, value>{};
  fields.emplace("name", value::make_string(field.name));
  fields.emplace("type_name",
                 value::make_string(render_type_expr(field.type.get())));
  fields.emplace("type_of", value::make_type_expr_fragment(field.type.get()));
  fields.emplace("is_data_member", value::make_bool(true));
  return value::make_struct("", std::move(fields));
}

/// Builds a `struct_instance` value describing one sum-type variant:
/// `{name: "some", payload_count: 1}` — the shape `static for v in
/// T.variants():` code is expected to destructure. Payload *types* aren't
/// exposed (unlike `make_field_descriptor`'s `type_name`): a sum-shaped
/// deriving body binds each payload positionally to a synthesized name
/// (`expr.ctor_pattern`, `eval.cpp`) and dispatches on it generically
/// (`==`, `.show()`, `ord_cmp`, ...) the same duck-typed way struct field
/// derivation already does, so nothing here needs to know the payload's
/// declared spelling.
[[nodiscard]] auto make_variant_descriptor(const ast::sum_variant &variant)
    -> value {
  auto fields = std::unordered_map<std::string, value>{};
  fields.emplace("name", value::make_string(variant.name));
  fields.emplace("payload_count", value::make_int(static_cast<int64_t>(
                                      variant.payload_types.size())));
  return value::make_struct("", std::move(fields));
}

/// Builds the `type_kind` value `T.kind()` returns
/// (`spec/specification/04-stdlib/type-traits/60-meta-queries.md`) — a
/// `variant_instance` of the prelude sum type `type_kind`
/// (`src/std/type_traits.kira`). `T.kind()` never fails, so this always
/// returns something, picking the closest kind when the narrower
/// classifications below don't apply:
///
/// - A real `type_decl` (from `resolve_type_reference`, i.e. a user type)
///   classifies precisely from its own `struct_type_def`/`sum_type_def`
///   shape.
/// - `@array_kind`/`@fn_kind` are recognized by the two builtin-generic
///   names `types_.entry` (`check.cpp`) is known to use for them when `T`
///   was bound from an ordinary generic instantiation with a concrete
///   compound type (see `checker::check_function`'s `type_param_locals`).
/// - Everything else — every builtin scalar, and any other compound
///   builtin (`slice`/`list`/`option`/`box`/a view) this narrow name-only
///   classifier has no declaration to distinguish from one — falls back to
///   `@scalar_kind`. A view in particular can never be told apart from its
///   referent this way: `&T` and `T` name-classify identically once
///   reduced to `{name, decl}`, since neither the evaluator nor `type_
///   value` carries the reference-ness of a bound type argument. Both
///   `@view_kind` and `@trait_kind`/`@module_kind` (the latter two never
///   reachable at all through this *type*-argument path) are therefore
///   real, documented gaps, not silent wrong answers within scope.
[[nodiscard]] auto make_type_kind_value(const ast::type_decl *decl,
                                        const std::string &name) -> value {
  if (decl != nullptr && decl->definition != nullptr) {
    if (decl->definition->kind == ast::node_kind::struct_type_def) {
      return value::make_variant("type_kind", "struct_kind", {});
    }
    if (decl->definition->kind == ast::node_kind::sum_type_def) {
      return value::make_variant("type_kind", "sum_kind", {});
    }
  }
  if (name == "array") {
    return value::make_variant("type_kind", "array_kind", {});
  }
  if (name == "fn") {
    return value::make_variant("type_kind", "fn_kind", {});
  }
  return value::make_variant("type_kind", "scalar_kind", {});
}

} // namespace

auto evaluator::try_eval_type_reflection_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  if (field.field_name != "fields" && field.field_name != "field_count" &&
      field.field_name != "name" && field.field_name != "variants" &&
      field.field_name != "variant_count" && field.field_name != "kind") {
    return std::nullopt;
  }
  const auto &object_ident =
      dynamic_cast<const ast::ident_expr &>(*field.object);
  const auto &type_name = object_ident.name;

  // `name`/`kind` only need a name — resolved through `resolve_generic_
  // type_arg`, which (unlike `resolve_type_reference` below) also answers
  // for a builtin scalar or a compound builtin bound as a generic type
  // parameter (see `checker::check_function`'s `type_param_locals`), since
  // neither of those has a `type_decl` to look up.
  if (field.field_name == "name" || field.field_name == "kind") {
    const auto resolved_arg = resolve_generic_type_arg(object_ident);
    if (!resolved_arg.has_value()) {
      return std::nullopt;
    }
    if (field.field_name == "name") {
      return value::make_string(resolved_arg->type_name);
    }
    return make_type_kind_value(resolved_arg->type_decl,
                                resolved_arg->type_name);
  }

  const auto *resolved = resolve_type_reference(object_ident);
  if (resolved == nullptr) {
    return std::nullopt;
  }
  const auto &decl = *resolved;

  if (field.field_name == "name") {
    return value::make_string(decl.name);
  }

  if (field.field_name == "variants" || field.field_name == "variant_count") {
    // Symmetric with the struct-only gate below: a struct type's "variants"
    // would mean something different (it has none), out of scope here.
    if (decl.definition == nullptr ||
        decl.definition->kind != ast::node_kind::sum_type_def) {
      return report(
          call.span,
          std::format("`{}.{}` is only supported for a sum-shaped type; `{}` "
                      "isn't one",
                      type_name, field.field_name, type_name));
    }
    const auto &sum_def =
        dynamic_cast<const ast::sum_type_def &>(*decl.definition);
    if (field.field_name == "variant_count") {
      return value::make_int(
          static_cast<int64_t>(sum_def.body.variants.size()));
    }
    auto elements = std::vector<value>{};
    elements.reserve(sum_def.body.variants.size());
    for (const auto &variant : sum_def.body.variants) {
      elements.push_back(make_variant_descriptor(variant));
    }
    return value::make_list(std::move(elements));
  }

  // `fields()`/`field_count()` only make sense for a struct-shaped type —
  // a sum type's "fields" would mean something different (variant
  // payloads), out of scope here; report clearly rather than silently
  // returning an empty/wrong answer.
  if (decl.definition == nullptr ||
      decl.definition->kind != ast::node_kind::struct_type_def) {
    return report(call.span,
                  std::format("`{}.{}` is only supported for a struct-shaped "
                              "type; `{}` isn't one",
                              type_name, field.field_name, type_name));
  }
  const auto &struct_def =
      dynamic_cast<const ast::struct_type_def &>(*decl.definition);

  if (field.field_name == "field_count") {
    return value::make_int(static_cast<int64_t>(struct_def.body.fields.size()));
  }

  // "fields"
  auto elements = std::vector<value>{};
  elements.reserve(struct_def.body.fields.size());
  for (const auto &field_decl : struct_def.body.fields) {
    elements.push_back(make_field_descriptor(field_decl));
  }
  return value::make_list(std::move(elements));
}

void evaluator::register_pending_type(std::string name,
                                      const ast::type_decl &decl) {
  pending_types_.emplace(std::move(name), &decl);
}

void evaluator::register_pending_module(std::string name,
                                        module_reflection_info info) {
  pending_modules_.insert_or_assign(std::move(name), std::move(info));
}

void evaluator::register_impl_coherence_info(impl_coherence_info info) {
  coherence_info_ = std::move(info);
}

namespace {

/// Builds a member descriptor value `{name: "...", is_pub: <bool>,
/// is_data_member: false}` — the shape `static for m in M.functions():`
/// code destructures. `is_pub` is exposed rather than pre-filtered so a
/// reflection consumer can decide which members it cares about (the
/// evaluator has no caller-module context with which to apply the "pub
/// from outside, all from inside" rule itself). `is_data_member` is always
/// `false` here (`M.functions()`/`M.types()` list `def`/`type` members,
/// never a struct field — see `member.is_data_member()`,
/// `spec/specification/04-stdlib/type-traits/60-meta-queries.md`), the
/// counterpart to `make_field_descriptor`'s always-`true`.
[[nodiscard]] auto
make_module_member_descriptor(const evaluator::module_member_info &member)
    -> value {
  auto fields = std::unordered_map<std::string, value>{};
  fields.emplace("name", value::make_string(member.name));
  fields.emplace("is_pub", value::make_bool(member.is_pub));
  fields.emplace("is_data_member", value::make_bool(false));
  return value::make_struct("", std::move(fields));
}

} // namespace

auto evaluator::try_eval_module_reflection_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  if (field.field_name != "name" && field.field_name != "functions" &&
      field.field_name != "types" && field.field_name != "function_count" &&
      field.field_name != "type_count") {
    return std::nullopt;
  }
  const auto &object_ident =
      dynamic_cast<const ast::ident_expr &>(*field.object);
  const auto it = pending_modules_.find(object_ident.name);
  if (it == pending_modules_.end()) {
    return std::nullopt; // not a registered module — let other dispatch try
  }
  const auto &info = it->second;

  if (field.field_name == "name") {
    return value::make_string(object_ident.name);
  }
  if (field.field_name == "function_count") {
    return value::make_int(static_cast<int64_t>(info.functions.size()));
  }
  if (field.field_name == "type_count") {
    return value::make_int(static_cast<int64_t>(info.types.size()));
  }

  const auto &members =
      field.field_name == "functions" ? info.functions : info.types;
  auto elements = std::vector<value>{};
  elements.reserve(members.size());
  for (const auto &member : members) {
    elements.push_back(make_module_member_descriptor(member));
  }
  return value::make_list(std::move(elements));
}

namespace {

/// The same key format `checker::type_key_of` (`check.cpp`) uses to index
/// `impl_trait_index_`/the reverse `traits_by_type_key` map mirrored into
/// `coherence_info_` — a user type's declaration address, or a builtin/
/// compound type's plain name. Reconstructed here rather than shared
/// directly since the evaluator has no dependency on `semantic::checker`;
/// keeping the two in lockstep is this function's whole job.
[[nodiscard]] auto type_key_of_value(const value &type_arg) -> std::string {
  if (type_arg.type_decl != nullptr) {
    return std::format("u:{}", static_cast<const void *>(type_arg.type_decl));
  }
  return std::format("n:{}", type_arg.type_name);
}

} // namespace

auto evaluator::try_eval_trait_reflection_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr ||
      call.callee->kind != ast::node_kind::field_expr) {
    return std::nullopt;
  }
  const auto &field = dynamic_cast<const ast::field_expr &>(*call.callee);
  if (field.object == nullptr ||
      field.object->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  if (field.field_name != "traits" && field.field_name != "requires") {
    return std::nullopt;
  }
  const auto &object_ident =
      dynamic_cast<const ast::ident_expr &>(*field.object);

  if (field.field_name == "traits") {
    const auto resolved_arg = resolve_generic_type_arg(object_ident);
    if (!resolved_arg.has_value()) {
      return std::nullopt;
    }
    auto elements = std::vector<value>{};
    if (const auto it = coherence_info_.traits_by_type_key.find(
            type_key_of_value(*resolved_arg));
        it != coherence_info_.traits_by_type_key.end()) {
      elements.reserve(it->second.size());
      for (const auto &trait_name : it->second) {
        elements.push_back(value::make_string(trait_name));
      }
    }
    return value::make_list(std::move(elements));
  }

  // `Trait.requires()` — traits have no separate registration table the
  // way types do (`pending_types_`), so `object_ident.name` is taken at
  // face value as the trait's own name; a trait with no `requires` clause
  // simply has no entry in `trait_requires`, giving the correct empty list
  // rather than a lookup failure.
  auto elements = std::vector<value>{};
  if (const auto it = coherence_info_.trait_requires.find(object_ident.name);
      it != coherence_info_.trait_requires.end()) {
    elements.reserve(it->second.size());
    for (const auto &supertrait_name : it->second) {
      elements.push_back(value::make_string(supertrait_name));
    }
  }
  return value::make_list(std::move(elements));
}

auto evaluator::try_eval_implements_call(const ast::call_expr &call)
    -> std::optional<value> {
  if (call.callee == nullptr) {
    return std::nullopt;
  }
  auto type_arg_exprs = std::vector<const ast::expr *>{};
  const auto *base =
      unwrap_explicit_generic_callee(*call.callee, type_arg_exprs);
  if (base == nullptr || base->kind != ast::node_kind::ident_expr) {
    return std::nullopt;
  }
  const auto &base_ident = dynamic_cast<const ast::ident_expr &>(*base);
  if (base_ident.name != "implements" || type_arg_exprs.size() != 2) {
    return std::nullopt;
  }
  if (type_arg_exprs[0]->kind != ast::node_kind::ident_expr ||
      type_arg_exprs[1]->kind != ast::node_kind::ident_expr) {
    return report(call.span,
                  "`implements[T, Trait]`'s arguments must be plain type/"
                  "trait names");
  }
  const auto &t_ident =
      dynamic_cast<const ast::ident_expr &>(*type_arg_exprs[0]);
  const auto &trait_ident =
      dynamic_cast<const ast::ident_expr &>(*type_arg_exprs[1]);
  const auto resolved_t = resolve_generic_type_arg(t_ident);
  if (!resolved_t.has_value()) {
    return report(
        t_ident.span,
        std::format("`{}` does not name a known type here", t_ident.name));
  }
  // Deliberately not diagnosed here when `trait_ident` names a higher-
  // kinded trait or a `concept` (the spec's stated error case): neither is
  // ever recorded in `traits_by_type_key` (only kind-`*` impls are, mirroring
  // `impl_trait_index_`'s own scope), so both simply answer `false` rather
  // than reporting — a real, narrower-than-spec answer rather than a wrong
  // one, but not the clear diagnostic the spec asks for.
  const auto it =
      coherence_info_.traits_by_type_key.find(type_key_of_value(*resolved_t));
  const auto has_trait = it != coherence_info_.traits_by_type_key.end() &&
                         std::ranges::contains(it->second, trait_ident.name);
  return value::make_bool(has_trait);
}

} // namespace kira::comptime
