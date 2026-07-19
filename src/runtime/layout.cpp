#include "src/runtime/layout.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "src/parser/ast.h"
#include "src/runtime/arena.h"

namespace kira::runtime {

namespace {

using semantic::type_entry;
using semantic::type_kind;
using semantic::type_table;

/// Unwraps `&T`/`&mut T` down to `T`. A reference's runtime representation
/// *is* its referent's — both are the same single pointer into the same
/// heap block (`layout.h`'s top comment, and `is_heap_pointer_value`'s
/// `addr_of`/`addr_of_mut` passthrough in both backends) — so every layout
/// question asked about `&T` has to be answered for `T`. The checker
/// records the unstripped `&T` on a `hir_field`/`hir_index`'s `object`
/// (`infer_index`/`infer_method_call` strip only for their own dispatch,
/// not for what they record), so the reference arrives here routinely:
/// `c.value = ...` through a `&mut cell` parameter reaches
/// `struct_field_offset` with `&mut cell`, not `cell`. Stripping centrally
/// here rather than at each backend call site keeps the two backends from
/// having to agree on it separately.
[[nodiscard]] auto strip_refs(const type_table &types, semantic::type_id id)
    -> semantic::type_id {
  const auto *entry = &types.entry(id);
  while (entry->kind == type_kind::ref_kind) {
    id = entry->result;
    entry = &types.entry(id);
  }
  return id;
}

[[nodiscard]] auto struct_fields_of(const type_entry &instance)
    -> const std::vector<ast::struct_field> * {
  if (instance.kind != type_kind::struct_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::struct_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::struct_type_def &>(*instance.decl->definition)
              .body.fields;
}

/// Builds a variant descriptor whose payload arity is `payload_slots` —
/// `payload_types`' *elements* are deliberately left null: nothing in this
/// file (or its callers in `bytecode_compiler`/`llvm_codegen`) ever resolves
/// a payload's type through this path, only its slot *count* (see this
/// file's top comment), so a null placeholder per slot is enough.
[[nodiscard]] auto make_variant(std::string name, size_t payload_slots)
    -> ast::sum_variant {
  ast::sum_variant variant;
  variant.name = std::move(name);
  variant.payload_types.resize(payload_slots);
  return variant;
}

/// `option[T]`/`result[T, E]` are builtin generics (`type_kind::
/// builtin_generic_kind`) with no backing `ast::type_decl` for
/// `sum_variants_of` below to walk — unlike a user sum type, their variant
/// shape isn't part of any declaration syntax to read back, so it's
/// hardcoded here instead. Declaration order and payload slots match
/// `semantic::check.cpp`'s own hardcoded handling of these two builtins
/// (`check_prelude_variant`, `check_constructor_pattern`,
/// `check_match_exhaustiveness`): `some`/`ok` carry the first generic
/// argument as a 1-slot payload, `err` carries the second, `none` carries
/// nothing.
[[nodiscard]] auto
make_two_variants(std::string first_name, size_t first_payload_slots,
                  std::string second_name, size_t second_payload_slots)
    -> std::vector<ast::sum_variant> {
  // `ast::sum_variant` holds a `vector<ptr<type_expr>>` and so is move-only
  // — build this with `push_back`/`std::move` rather than a
  // `std::initializer_list` braced-init, which would need to copy.
  auto variants = std::vector<ast::sum_variant>{};
  variants.push_back(make_variant(std::move(first_name), first_payload_slots));
  variants.push_back(
      make_variant(std::move(second_name), second_payload_slots));
  return variants;
}

[[nodiscard]] auto builtin_generic_variants_of(const type_entry &instance)
    -> const std::vector<ast::sum_variant> * {
  if (instance.kind != type_kind::builtin_generic_kind) {
    return nullptr;
  }
  static const auto option_variants = make_two_variants("some", 1, "none", 0);
  static const auto result_variants = make_two_variants("ok", 1, "err", 1);
  if (instance.name == "option") {
    return &option_variants;
  }
  if (instance.name == "result") {
    return &result_variants;
  }
  return nullptr;
}

[[nodiscard]] auto sum_variants_of(const type_entry &instance)
    -> const std::vector<ast::sum_variant> * {
  if (const auto *builtin = builtin_generic_variants_of(instance);
      builtin != nullptr) {
    return builtin;
  }
  if (instance.kind != type_kind::sum_kind || instance.decl == nullptr ||
      instance.decl->definition == nullptr ||
      instance.decl->definition->kind != ast::node_kind::sum_type_def) {
    return nullptr;
  }
  return &dynamic_cast<const ast::sum_type_def &>(*instance.decl->definition)
              .body.variants;
}

} // namespace

auto struct_field_names(const type_table &types, semantic::type_id id)
    -> std::vector<std::string_view> {
  id = strip_refs(types, id);
  auto result = std::vector<std::string_view>{};
  const auto *fields = struct_fields_of(types.entry(id));
  if (fields == nullptr) {
    return result;
  }
  result.reserve(fields->size());
  for (const auto &field : *fields) {
    result.push_back(field.name);
  }
  return result;
}

auto struct_field_slot(const type_table &types, semantic::type_id id,
                       std::string_view name) -> std::optional<size_t> {
  id = strip_refs(types, id);
  const auto *fields = struct_fields_of(types.entry(id));
  if (fields == nullptr) {
    return std::nullopt;
  }
  for (size_t i = 0; i < fields->size(); ++i) {
    if ((*fields)[i].name == name) {
      return i;
    }
  }
  return std::nullopt;
}

auto sum_variant_names(const type_table &types, semantic::type_id id)
    -> std::vector<std::string_view> {
  id = strip_refs(types, id);
  auto result = std::vector<std::string_view>{};
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return result;
  }
  result.reserve(variants->size());
  for (const auto &variant : *variants) {
    result.push_back(variant.name);
  }
  return result;
}

auto sum_variant_tag(const type_table &types, semantic::type_id id,
                     std::string_view name) -> std::optional<int64_t> {
  id = strip_refs(types, id);
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return std::nullopt;
  }
  for (size_t i = 0; i < variants->size(); ++i) {
    if ((*variants)[i].name == name) {
      return static_cast<int64_t>(i);
    }
  }
  return std::nullopt;
}

auto sum_variant_payload_slots(const type_table &types, semantic::type_id id,
                               std::string_view name) -> std::optional<size_t> {
  id = strip_refs(types, id);
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return std::nullopt;
  }
  for (const auto &variant : *variants) {
    if (variant.name == name) {
      return variant.payload_types.size();
    }
  }
  return std::nullopt;
}

auto sum_max_payload_slots(const type_table &types, semantic::type_id id)
    -> size_t {
  id = strip_refs(types, id);
  const auto *variants = sum_variants_of(types.entry(id));
  if (variants == nullptr) {
    return 0;
  }
  auto max_slots = size_t{0};
  for (const auto &variant : *variants) {
    if (variant.payload_types.size() > max_slots) {
      max_slots = variant.payload_types.size();
    }
  }
  return max_slots;
}

namespace {

/// Rounds `offset` up to the next multiple of `align` (`align` is always a
/// power of two here — every alignment this file ever produces comes from
/// `scalar_layout`/the fixed 8-byte heap-pointer alignment).
[[nodiscard]] constexpr auto round_up(size_t offset, size_t align) -> size_t {
  return (offset + align - 1) / align * align;
}

/// Natural size/alignment table for builtin scalars — mirrors
/// `bytecode::numeric_kind_of`'s name-to-representation mapping
/// (`src/bytecode/value.cpp`) exactly, but expressed as byte size/align
/// rather than a `numeric_kind`, since `runtime` sits below `bytecode` in
/// the dependency graph (`bytecode`'s `vm` library depends on `runtime`,
/// not the reverse) and so can't reuse that table directly.
[[nodiscard]] auto scalar_layout(std::string_view name)
    -> std::optional<layout_info> {
  if (name == "bool" || name == "int8" || name == "uint8" || name == "byte") {
    return layout_info{.size_bytes = 1, .align_bytes = 1};
  }
  if (name == "int16" || name == "uint16") {
    return layout_info{.size_bytes = 2, .align_bytes = 2};
  }
  if (name == "int32" || name == "uint32" || name == "char") {
    return layout_info{.size_bytes = 4, .align_bytes = 4};
  }
  if (name == "int64" || name == "uint64" || name == "isize" ||
      name == "usize" || name == "float64") {
    return layout_info{.size_bytes = 8, .align_bytes = 8};
  }
  if (name == "float32") {
    return layout_info{.size_bytes = 4, .align_bytes = 4};
  }
  if (name == "unit") {
    return layout_info{.size_bytes = 0, .align_bytes = 1};
  }
  // int128/uint128/float128 and every non-scalar builtin (str is handled by
  // the heap-pointer branch in `layout_of` before this is reached) fall
  // through to `nullopt` — same 128-bit gap `numeric_kind_of` documents.
  return std::nullopt;
}

/// Whether `entry`'s kind is one of the heap-referenced representations
/// (`src/runtime/layout.h`'s top comment) that is pointer-sized/aligned as
/// a field/element of another aggregate.
[[nodiscard]] auto is_heap_kind(const type_entry &entry) -> bool {
  switch (entry.kind) {
  case type_kind::tuple_kind:
  case type_kind::array_kind:
  case type_kind::struct_kind:
  case type_kind::sum_kind:
  case type_kind::opaque_kind:
  case type_kind::fn_kind:
  case type_kind::ref_kind:
  case type_kind::ptr_kind:
  case type_kind::builtin_generic_kind:
    return true;
  case type_kind::builtin_kind:
    return entry.name == "str";
  default:
    return false;
  }
}

/// Resolves one struct field's declared type_expr to a `layout_info`
/// *without* the checker's full generic-substitution machinery
/// (`semantic::check.cpp`'s private `resolve_type`, which needs a live
/// `checker` instance this module doesn't have — see `types.h`'s own doc
/// comment on why field *types* aren't otherwise exposed). This is possible
/// because layout only cares about a field's top-level size/alignment, and
/// every compound/aggregate type (struct, sum, tuple, array, `list[T]`, a
/// qualified/generic-argumented named type, `&T`/`*T`, `fn(...)`) is
/// uniformly pointer-sized/aligned as a field regardless of what it
/// contains — the one case that genuinely needs the *instance*'s own
/// substitution is a field typed as a bare, unqualified, argument-less name
/// that is itself one of the struct's own generic parameters (`T`), handled
/// below by indexing `instance.args` directly rather than a general
/// substitution walk.
[[nodiscard]] auto field_layout(const type_table &types,
                                const type_entry &instance,
                                const ast::type_expr &expr)
    -> std::optional<layout_info>;

[[nodiscard]] auto layout_of_entry(const type_entry &entry)
    -> std::optional<layout_info> {
  if (entry.kind == type_kind::builtin_kind) {
    if (entry.name == "str") {
      return layout_info{.size_bytes = 8, .align_bytes = 8};
    }
    return scalar_layout(entry.name);
  }
  if (is_heap_kind(entry)) {
    return layout_info{.size_bytes = 8, .align_bytes = 8};
  }
  return std::nullopt;
}

[[nodiscard]] auto field_layout(const type_table &types,
                                const type_entry &instance,
                                const ast::type_expr &expr)
    -> std::optional<layout_info> {
  if (expr.has_error) {
    return std::nullopt;
  }
  if (expr.kind != ast::node_kind::named_type) {
    // Every other type_expr shape (tuple/slice/array/ref/ptr/fn/quote/
    // union/refinement) denotes a heap-referenced or pointer-sized
    // representation as a field, regardless of its contents.
    return layout_info{.size_bytes = 8, .align_bytes = 8};
  }
  const auto &named = dynamic_cast<const ast::named_type &>(expr);
  if (named.path.size() != 1 || !named.type_args.empty()) {
    // A qualified path (`std.foo.bar`) or a generic application
    // (`list[T]`, `option[int32]`, a user generic struct/sum) — all
    // heap-referenced as a field.
    return layout_info{.size_bytes = 8, .align_bytes = 8};
  }
  const auto &name = named.path.front();
  if (const auto scalar = scalar_layout(name); scalar.has_value()) {
    return scalar;
  }
  if (name == "str") {
    return layout_info{.size_bytes = 8, .align_bytes = 8};
  }
  // A refinement lays out as the type it refines — the predicate is a
  // compile-time fact with no representation of its own — so a field typed
  // `positive` occupies exactly what its `int32` base does, not the 8-byte
  // heap slot the fallback below would give it. That distinction is invisible
  // in an ordinary struct (writer and reader agree either way) and load-
  // bearing in a `packed` one.
  if (const auto base = types.refinement_base_named(name)) {
    return layout_of(types, *base);
  }
  // A bare single-segment name that isn't a scalar/`str` is either one of
  // the struct's own generic parameters (needs `instance.args`
  // substitution) or another named type (user struct/sum/alias, always
  // heap-referenced as a field).
  if (instance.decl != nullptr) {
    for (size_t i = 0; i < instance.decl->type_params.size(); ++i) {
      const auto &param = instance.decl->type_params[i];
      if (param.is_value_param || param.name != name) {
        continue;
      }
      if (i >= instance.args.size()) {
        return std::nullopt; // unresolved generic parameter
      }
      return layout_of(types, instance.args[i]);
    }
  }
  return layout_info{.size_bytes = 8, .align_bytes = 8};
}

/// Shared padded/packed field walk driving both `struct_layout` and
/// `struct_field_offset` — invokes `visit(field_index, offset, field_size)`
/// for each field of struct-kind `id` in declaration order (packed or
/// padded per `is_struct_packed`), returning the whole struct's final
/// `layout_info`, or `nullopt` if `id` isn't a struct or any field's own
/// layout is unrepresentable.
template <typename Visit>
[[nodiscard]] auto walk_struct_layout(const type_table &types,
                                      semantic::type_id id, Visit &&visit)
    -> std::optional<layout_info> {
  const auto &instance = types.entry(id);
  const auto *fields = struct_fields_of(instance);
  if (fields == nullptr) {
    return std::nullopt;
  }
  const auto packed = is_struct_packed(types, id);
  auto offset = size_t{0};
  auto max_align = size_t{1};
  for (size_t i = 0; i < fields->size(); ++i) {
    const auto &field = (*fields)[i];
    if (field.type == nullptr) {
      return std::nullopt;
    }
    const auto field_layout_info = field_layout(types, instance, *field.type);
    if (!field_layout_info.has_value()) {
      return std::nullopt;
    }
    if (!packed) {
      max_align = std::max(max_align, field_layout_info->align_bytes);
      offset = round_up(offset, field_layout_info->align_bytes);
    }
    visit(i, offset, field_layout_info->size_bytes);
    offset += field_layout_info->size_bytes;
  }
  const auto size = packed ? offset : round_up(offset, max_align);
  return layout_info{.size_bytes = size,
                     .align_bytes = packed ? size_t{1} : max_align};
}

} // namespace

auto layout_of(const type_table &types, semantic::type_id id)
    -> std::optional<layout_info> {
  id = strip_refs(types, id);
  // A refinement is its base at runtime — the predicate is a compile-time fact
  // with no representation of its own (`spec/dependent-types-design.md` 3.1),
  // so a `positive` lays out exactly as the `int32` it refines. The checker
  // erases refinements from every type it hands downstream
  // (`type_table::erase_refinements`), so this should already be unreachable;
  // it is here so that a layout query made straight against the type table
  // — as `struct_layout` does for a substituted generic argument — cannot
  // silently answer "heap pointer" for a value that is really a machine word.
  return layout_of_entry(types.entry(types.strip_refinement(id)));
}

auto is_struct_packed(const type_table &types, semantic::type_id id) -> bool {
  id = strip_refs(types, id);
  const auto &entry = types.entry(id);
  if (entry.kind != type_kind::struct_kind || entry.decl == nullptr) {
    return false;
  }
  return entry.decl->modifiers.is_packed;
}

auto struct_layout(const type_table &types, semantic::type_id id)
    -> layout_info {
  id = strip_refs(types, id);
  const auto result =
      walk_struct_layout(types, id, [](size_t, size_t, size_t) -> void {});
  return result.value_or(layout_info{});
}

auto struct_field_offset(const type_table &types, semantic::type_id id,
                         std::string_view name) -> std::optional<size_t> {
  id = strip_refs(types, id);
  auto found = std::optional<size_t>{};
  const auto result = walk_struct_layout(
      types, id, [&](size_t index, size_t offset, size_t) -> void {
        const auto *fields = struct_fields_of(types.entry(id));
        if (fields != nullptr && (*fields)[index].name == name) {
          found = offset;
        }
      });
  if (!result.has_value()) {
    return std::nullopt;
  }
  return found;
}

auto list_reserve_slot(uint64_t *header, size_t elem_size) -> void * {
  const auto len = header[0];
  auto cap = header[1];
  if (len >= cap) {
    const auto new_cap = cap == 0 ? uint64_t{4} : cap * 2;
    auto *new_data =
        static_cast<uint8_t *>(global_arena().allocate(new_cap * elem_size));
    if (cap > 0) {
      const auto *old_data =
          reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(header[2]));
      std::copy(old_data, old_data + (len * elem_size), new_data);
    }
    header[1] = new_cap;
    header[2] = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(new_data));
  }
  auto *data = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(header[2]));
  header[0] = len + 1;
  return data + (len * elem_size);
}

extern "C" auto kira_rt_list_reserve_slot(uint64_t *header, uint64_t elem_size)
    -> void * {
  return list_reserve_slot(header, elem_size);
}

} // namespace kira::runtime
