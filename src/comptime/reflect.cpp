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
  const auto &type_name =
      dynamic_cast<const ast::ident_expr &>(*field.object).name;
  const auto it = pending_types_.find(type_name);
  if (it == pending_types_.end() || it->second == nullptr) {
    return std::nullopt;
  }
  const auto &decl = *it->second;

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

} // namespace kira::comptime
