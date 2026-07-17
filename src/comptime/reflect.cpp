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
/// type_name: "int32"}` — the shape `static for field in T.fields():` code
/// is expected to destructure. Named `type_name`, not `type`: `type` is a
/// reserved keyword in Kira, so a quoted `field.type` access could never
/// parse.
[[nodiscard]] auto make_field_descriptor(const ast::struct_field &field)
    -> value {
  auto fields = std::unordered_map<std::string, value>{};
  fields.emplace("name", value::make_string(field.name));
  fields.emplace("type_name",
                 value::make_string(render_type_expr(field.type.get())));
  return value::make_struct("", std::move(fields));
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
      field.field_name != "name") {
    return std::nullopt;
  }
  const auto &object_ident =
      dynamic_cast<const ast::ident_expr &>(*field.object);
  const auto &type_name = object_ident.name;
  const auto *resolved = resolve_type_reference(object_ident);
  if (resolved == nullptr) {
    return std::nullopt;
  }
  const auto &decl = *resolved;

  if (field.field_name == "name") {
    return value::make_string(decl.name);
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

namespace {

/// Builds a member descriptor value `{name: "...", is_pub: <bool>}` — the
/// shape `static for m in M.functions():` code destructures. `is_pub` is
/// exposed rather than pre-filtered so a reflection consumer can decide which
/// members it cares about (the evaluator has no caller-module context with
/// which to apply the "pub from outside, all from inside" rule itself).
[[nodiscard]] auto
make_module_member_descriptor(const evaluator::module_member_info &member)
    -> value {
  auto fields = std::unordered_map<std::string, value>{};
  fields.emplace("name", value::make_string(member.name));
  fields.emplace("is_pub", value::make_bool(member.is_pub));
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

} // namespace kira::comptime
