#include "types.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "src/semantic/module_index.h"

namespace kira::semantic {
namespace {

/// Every scalar name recognized as a builtin type (numeric family, `bool`,
/// `str`, quote-value types, and the two built-in execution contexts).
constexpr std::array<std::string_view, 27> k_builtin_scalar_names = {
    "bool",   "char",    "str",     "unit",   "never",  "byte",
    "int8",   "int16",   "int32",   "int64",  "int128", "uint8",
    "uint16", "uint32",  "uint64",  "uint128", "float32", "float64",
    "float128", "isize", "usize",   "ordering", "io",   "cpu",
    "expr",   "stmt",    "def_expr",
};

/// One prelude container name and its accepted generic-argument count
/// range (inclusive).
struct generic_arity_entry {
  std::string_view name;
  size_t min_args;
  size_t max_args;
};

/// Prelude container names and their allowed generic-argument arities.
constexpr std::array<generic_arity_entry, 12> k_builtin_generic_arities = {{
    {.name="list", .min_args=1, .max_args=1},
    {.name="option", .min_args=1, .max_args=1},
    {.name="result", .min_args=2, .max_args=2},
    {.name="box", .min_args=1, .max_args=1},
    {.name="slice", .min_args=1, .max_args=1},
    {.name="slice_mut", .min_args=1, .max_args=1},
    {.name="shared", .min_args=0, .max_args=1},
    {.name="task", .min_args=1, .max_args=3},
    {.name="channel", .min_args=1, .max_args=1},
    {.name="watch", .min_args=1, .max_args=1},
    {.name="mutex", .min_args=1, .max_args=1},
    {.name="atomic", .min_args=1, .max_args=1},
}};

/// Trait names available from the prelude without any `use`.
constexpr std::array<std::string_view, 12> k_prelude_trait_names = {
    "eq",  "ord", "hash", "show", "from", "into",
    "add", "sub", "mul",  "div",  "rem",  "neg",
};

/// Appends a bracketed, comma-separated list of `args` ids to `key`, for
/// building a unique intern key that distinguishes generic instantiations.
auto append_args_key(std::string &key, const std::vector<type_id> &args)
    -> void {
  key += '[';
  for (const auto arg : args) {
    key += std::to_string(arg);
    key += ',';
  }
  key += ']';
}

} // namespace

/// Reserves ids 0 and 1 for `k_unknown_type` and `k_error_type`.
type_table::type_table() {
  entries_.push_back(type_entry{.kind = type_kind::unknown_kind,
                                .name = "<unknown>"});
  entries_.push_back(type_entry{.kind = type_kind::error_kind,
                                .name = "<error>"});
}

/// Structural interning keyed on `key`: identical keys always produce the
/// same id, so callers build `key` to encode everything that should make
/// two types distinct (kind tag, name/declaration identity, arguments).
auto type_table::intern(std::string key, type_entry entry) -> type_id {
  if (const auto it = interned_.find(key); it != interned_.end()) {
    return it->second;
  }
  const auto id = static_cast<type_id>(entries_.size());
  entries_.push_back(std::move(entry));
  interned_.emplace(std::move(key), id);
  return id;
}

/// Interns using name alone as the key — every `int32` is the same type.
auto type_table::builtin(std::string_view name) -> type_id {
  return intern(std::format("b:{}", name),
                type_entry{.kind = type_kind::builtin_kind,
                           .name = std::string(name)});
}

/// Interns using name plus argument ids as the key, so `list[int32]` and
/// `list[str]` are distinct but any two `list[int32]`s are the same type.
auto type_table::builtin_generic(std::string_view name,
                                 std::vector<type_id> args) -> type_id {
  auto key = std::format("g:{}", name);
  append_args_key(key, args);
  return intern(std::move(key),
                type_entry{.kind = type_kind::builtin_generic_kind,
                           .name = std::string(name),
                           .args = std::move(args)});
}

/// Interns using element ids as the key, so tuple identity is purely
/// structural (element types and arity), independent of source location.
auto type_table::tuple_of(std::vector<type_id> elements) -> type_id {
  auto key = std::string("t:");
  append_args_key(key, elements);
  return intern(std::move(key), type_entry{.kind = type_kind::tuple_kind,
                                           .name = "tuple",
                                           .args = std::move(elements)});
}

/// Interns using the element id and the length (or `?` when not statically
/// known) as the key.
auto type_table::array_of(type_id element, std::optional<uint64_t> size)
    -> type_id {
  auto key = std::format("a:{}:{}", element,
                         size.has_value() ? std::to_string(*size) : "?");
  return intern(std::move(key), type_entry{.kind = type_kind::array_kind,
                                           .name = "array",
                                           .result = element,
                                           .array_size = size});
}

/// Interns using parameter ids and the result id as the key.
auto type_table::fn_of(std::vector<type_id> params, type_id result)
    -> type_id {
  auto key = std::string("f:");
  append_args_key(key, params);
  key += std::format("->{}", result);
  return intern(std::move(key), type_entry{.kind = type_kind::fn_kind,
                                           .name = "fn",
                                           .args = std::move(params),
                                           .result = result});
}

/// Interns using the inner id and mutability as the key.
auto type_table::ref_to(type_id inner, bool is_mut) -> type_id {
  return intern(std::format("r:{}:{}", inner, is_mut),
                type_entry{.kind = type_kind::ref_kind,
                           .name = "ref",
                           .result = inner,
                           .is_mut = is_mut});
}

/// Interns using the inner id and mutability as the key.
auto type_table::ptr_to(type_id inner, bool is_mut) -> type_id {
  return intern(std::format("p:{}:{}", inner, is_mut),
                type_entry{.kind = type_kind::ptr_kind,
                           .name = "ptr",
                           .result = inner,
                           .is_mut = is_mut});
}

/// Interns using the declaration's address and argument ids as the key, so
/// two instantiations of the same generic `type` declaration with the same
/// arguments are the same type, while distinct declarations with the same
/// name (in different modules) never collide. The concrete `type_kind` is
/// inferred from the declaration's body (struct, sum, or opaque/alias).
auto type_table::user_type(const ast::type_decl &decl,
                           std::string_view module_name,
                           std::vector<type_id> args) -> type_id {
  auto kind = type_kind::opaque_kind;
  if (decl.definition != nullptr) {
    if (decl.definition->kind == ast::node_kind::struct_type_def) {
      kind = type_kind::struct_kind;
    } else if (decl.definition->kind == ast::node_kind::sum_type_def) {
      kind = type_kind::sum_kind;
    }
  }
  auto key = std::format("u:{}", static_cast<const void *>(&decl));
  append_args_key(key, args);
  return intern(std::move(key), type_entry{.kind = kind,
                                           .name = decl.name,
                                           .module_name =
                                               std::string(module_name),
                                           .decl = &decl,
                                           .args = std::move(args)});
}

/// Interns using name alone as the key, so same-named type parameters in
/// the same scope collapse to one id.
auto type_table::type_param(std::string_view name) -> type_id {
  return intern(std::format("v:{}", name),
                type_entry{.kind = type_kind::type_param_kind,
                           .name = std::string(name)});
}

/// Bounds-checks `id`, falling back to the `unknown` entry rather than
/// indexing out of range.
auto type_table::entry(type_id id) const -> const type_entry & {
  if (static_cast<size_t>(id) >= entries_.size()) {
    return entries_[k_unknown_type];
  }
  return entries_[id];
}

/// Recursively renders `id` into Kira's own type syntax, matching the
/// spellings used in error messages (`_` for `unknown`, `list[int32]`,
/// `fn(int32) -> str`, `&mut T`, ...).
auto type_table::display(type_id id) const -> std::string {
  const auto &item = entry(id);
  switch (item.kind) {
  case type_kind::unknown_kind:
    return "_";
  case type_kind::error_kind:
    return "<error>";
  case type_kind::builtin_kind:
  case type_kind::type_param_kind:
    return item.name;
  case type_kind::builtin_generic_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    if (item.args.empty()) {
      return item.name;
    }
    auto out = item.name + "[";
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += "]";
    return out;
  }
  case type_kind::tuple_kind: {
    auto out = std::string("(");
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += ")";
    return out;
  }
  case type_kind::array_kind:
    return std::format("array[{}, {}]", display(item.result),
                       item.array_size.has_value()
                           ? std::to_string(*item.array_size)
                           : std::string("_"));
  case type_kind::fn_kind: {
    auto out = std::string("fn(");
    for (size_t i = 0; i < item.args.size(); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += display(item.args[i]);
    }
    out += std::format(") -> {}", display(item.result));
    return out;
  }
  case type_kind::ref_kind:
    return std::format("&{}{}", item.is_mut ? "mut " : "",
                       display(item.result));
  case type_kind::ptr_kind:
    return std::format("*{}{}", item.is_mut ? "mut " : "",
                       display(item.result));
  }
  return "<?>";
}

/// See the header for semantics.
auto type_table::is_unknown(type_id id) const -> bool {
  const auto kind = entry(id).kind;
  return kind == type_kind::unknown_kind || kind == type_kind::error_kind ||
         kind == type_kind::type_param_kind;
}

/// See the header for semantics.
auto type_table::is_boolean(type_id id) const -> bool {
  const auto &item = entry(id);
  return item.kind == type_kind::builtin_kind && item.name == "bool";
}

/// See the header for semantics.
auto type_table::is_integer(type_id id) const -> bool {
  const auto &item = entry(id);
  if (item.kind != type_kind::builtin_kind) {
    return false;
  }
  return item.name.starts_with("int") || item.name.starts_with("uint") ||
         item.name == "byte" || item.name == "isize" || item.name == "usize";
}

/// See the header for semantics.
auto type_table::is_float(type_id id) const -> bool {
  const auto &item = entry(id);
  return item.kind == type_kind::builtin_kind &&
         item.name.starts_with("float");
}

/// See the header for semantics.
auto type_table::is_numeric(type_id id) const -> bool {
  return is_integer(id) || is_float(id);
}

/// See the header for semantics.
auto type_table::is_unit(type_id id) const -> bool {
  const auto &item = entry(id);
  return item.kind == type_kind::builtin_kind && item.name == "unit";
}

/// Structural compatibility: identical ids always match; `unknown`/`error`/
/// type-parameter types match anything; `never` (from `panic` or an early
/// return) matches any expected type; a reference on either side compares
/// against its target type; and same-kind types compare their
/// declaration/name identity plus recursively-compatible arguments.
auto type_table::compatible(type_id expected, type_id found) const -> bool {
  if (expected == found || is_unknown(expected) || is_unknown(found)) {
    return true;
  }

  const auto &expected_entry = entry(expected);
  const auto &found_entry = entry(found);

  // `never` (panic, propagating return) is acceptable anywhere.
  if (found_entry.kind == type_kind::builtin_kind &&
      found_entry.name == "never") {
    return true;
  }

  if (expected_entry.kind != found_entry.kind) {
    // A `&T` argument position accepts a lent `T`; the borrow checker owns
    // the deeper rules, so typing treats the reference as its target here.
    if (expected_entry.kind == type_kind::ref_kind) {
      return compatible(expected_entry.result, found);
    }
    if (found_entry.kind == type_kind::ref_kind) {
      return compatible(expected, found_entry.result);
    }
    return false;
  }

  switch (expected_entry.kind) {
  case type_kind::builtin_generic_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind: {
    const auto same_declaration =
        expected_entry.decl != nullptr
            ? expected_entry.decl == found_entry.decl
            : expected_entry.name == found_entry.name;
    if (!same_declaration || expected_entry.args.size() != found_entry.args.size()) {
      return false;
    }
    for (size_t i = 0; i < expected_entry.args.size(); ++i) {
      if (!compatible(expected_entry.args[i], found_entry.args[i])) {
        return false;
      }
    }
    return true;
  }
  case type_kind::tuple_kind:
  case type_kind::fn_kind: {
    if (expected_entry.args.size() != found_entry.args.size() ||
        !compatible(expected_entry.result, found_entry.result)) {
      return false;
    }
    for (size_t i = 0; i < expected_entry.args.size(); ++i) {
      if (!compatible(expected_entry.args[i], found_entry.args[i])) {
        return false;
      }
    }
    return true;
  }
  case type_kind::array_kind:
    return compatible(expected_entry.result, found_entry.result) &&
           (!expected_entry.array_size.has_value() ||
            !found_entry.array_size.has_value() ||
            *expected_entry.array_size == *found_entry.array_size);
  case type_kind::ref_kind:
  case type_kind::ptr_kind:
    return compatible(expected_entry.result, found_entry.result) &&
           (found_entry.is_mut || !expected_entry.is_mut);
  default:
    return false;
  }
}

/// Linear search over `k_builtin_scalar_names`.
auto is_builtin_scalar_name(std::string_view name) -> bool {
  return std::ranges::any_of(k_builtin_scalar_names,
                      [name](std::string_view candidate)->  bool  {
                        return candidate == name;
                      });
}

/// Linear search over `k_builtin_generic_arities`.
auto builtin_generic_arity(std::string_view name)
    -> std::optional<std::pair<size_t, size_t>> {
  for (const auto &candidate : k_builtin_generic_arities) {
    if (candidate.name == name) {
      return std::pair{candidate.min_args, candidate.max_args};
    }
  }
  return std::nullopt;
}

/// 128-bit integer types have no representable max in `uint64_t` and are
/// intentionally left unhandled (returning `nullopt`), so literal-fit
/// checking simply does not fire for them yet.
auto integer_max_value(std::string_view name) -> std::optional<uint64_t> {
  if (name == "int8") {
    return 127u;
  }
  if (name == "int16") {
    return 32767u;
  }
  if (name == "int32") {
    return 2147483647u;
  }
  if (name == "int64" || name == "isize") {
    return 9223372036854775807u;
  }
  if (name == "uint8" || name == "byte") {
    return 255u;
  }
  if (name == "uint16") {
    return 65535u;
  }
  if (name == "uint32") {
    return 4294967295u;
  }
  if (name == "uint64" || name == "usize") {
    return 18446744073709551615u;
  }
  return std::nullopt;
}

/// Linear search over `k_prelude_trait_names`.
auto is_prelude_trait_name(std::string_view name) -> bool {
  return std::ranges::any_of(k_prelude_trait_names, [name](std::string_view candidate) -> bool -> bool {
    return candidate == name;
  });
}

// ==========================================================================
//  Program index construction
// ==========================================================================

namespace {

/// Expands one `use` declaration into the local name bindings it
/// introduces: the trailing path segment for a bare `use a.b.c`, one
/// binding per selected/renamed item for `use a.b.{c, d as e}`, or a single
/// wildcard marker for `use a.b.*`.
auto record_use_bindings(const ast::use_decl &decl, file_id_type file_id,
                         program_index &index) -> void {
  auto &bindings = index.imports[file_id];

  if (!decl.selector.has_value()) {
    if (decl.path.empty()) {
      return;
    }
    bindings.push_back(import_binding{
        .local_name = decl.path.back(),
        .path = decl.path,
        .leaf_name = {},
        .is_wildcard = false,
        .span = decl.span,
    });
    return;
  }

  if (decl.selector->kind == ast::UseSelectorKind::Wildcard) {
    bindings.push_back(import_binding{
        .local_name = {},
        .path = decl.path,
        .leaf_name = {},
        .is_wildcard = true,
        .span = decl.selector->span,
    });
    return;
  }

  for (const auto &item : decl.selector->items) {
    bindings.push_back(import_binding{
        .local_name = item.alias.value_or(item.name),
        .path = decl.path,
        .leaf_name = item.name,
        .is_wildcard = false,
        .span = item.span,
    });
  }
}

/// Classifies one module-scope item into `members`' matching table (types,
/// traits, concepts, functions, static bindings, impls), also indexing a sum
/// type's variants by name so a bare `@variant` can be found later without
/// knowing which sum type it belongs to.
auto record_module_item(const ast::node &item, file_id_type file_id,
                        module_members &members) -> void {
  switch (item.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = dynamic_cast<const ast::type_decl &>(item);
    if (decl.name.empty()) {
      return;
    }
    members.types.emplace(decl.name,
                          type_decl_ref{.decl = &decl, .file_id = file_id});
    if (decl.definition != nullptr &&
        decl.definition->kind == ast::node_kind::sum_type_def) {
      const auto &sum =
          dynamic_cast<const ast::sum_type_def &>(*decl.definition);
      for (const auto &variant : sum.body.variants) {
        if (!variant.name.empty()) {
          members.variants.emplace(
              variant.name,
              variant_ref{.sum_decl = &decl, .variant = &variant});
        }
      }
    }
    return;
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = dynamic_cast<const ast::trait_decl &>(item);
    if (!decl.name.empty()) {
      members.traits.emplace(decl.name,
                             trait_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = dynamic_cast<const ast::concept_decl &>(item);
    if (!decl.name.empty()) {
      members.concepts.emplace(
          decl.name, concept_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::func_decl: {
    const auto &decl = dynamic_cast<const ast::func_decl &>(item);
    if (!decl.name.empty()) {
      members.functions.emplace(
          decl.name, func_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::static_decl: {
    const auto &decl = dynamic_cast<const ast::static_decl &>(item);
    if (decl.decl_kind == ast::static_decl_kind::binding && !decl.name.empty()) {
      members.statics.emplace(
          decl.name, static_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &decl = dynamic_cast<const ast::impl_decl &>(item);
    members.impls.push_back(impl_ref{.decl = &decl,
                                     .module_name = members.module_name,
                                     .file_id = file_id});
    return;
  }
  default:
    return;
  }
}

/// Recursively indexes `items` into `index`: `use` declarations become
/// import bindings, inline submodules recurse under their qualified name,
/// and everything else is classified via `record_module_item`.
auto index_items(const std::vector<ast::ptr<ast::node>> &items,
                 std::string_view module_name, file_id_type file_id,
                 program_index &index) -> void {
  auto &members = index.modules[std::string(module_name)];
  members.module_name = std::string(module_name);

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (item->kind == ast::node_kind::use_decl) {
      record_use_bindings(dynamic_cast<const ast::use_decl &>(*item), file_id,
                          index);
      continue;
    }
    if (item->kind == ast::node_kind::sub_module_decl) {
      const auto &decl = dynamic_cast<const ast::sub_module_decl &>(*item);
      if (!decl.items.empty()) {
        index_items(decl.items, append_module_name(module_name, decl.name),
                    file_id, index);
      }
      continue;
    }
    record_module_item(*item, file_id, members);
  }
}

} // namespace

/// Exact-match lookup in the `modules` map.
auto program_index::find_module(std::string_view module_name) const
    -> const module_members * {
  const auto it = modules.find(std::string(module_name));
  return it != modules.end() ? &it->second : nullptr;
}

/// Skips files without a valid module declaration, then indexes the rest
/// via `index_items`.
auto build_program_index(const std::vector<parsed_module> &inputs)
    -> program_index {
  auto index = program_index{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }
    const auto module_name =
        join_strings(input.ast_file->module_decl->path, ".");
    index_items(input.ast_file->items, module_name, input.file_id, index);
  }

  return index;
}

} // namespace kira::semantic
