#include "src/semantic/program_index.h"

#include <string>

#include "src/semantic/module_index.h"
#include "src/parser/ast_traversal.h"

namespace cinder::semantic {
namespace {

/// Expands one `use` declaration into the local name bindings it
/// introduces: the trailing path segment for a bare `use a.b.c`, one
/// binding per selected/renamed item for `use a.b.{c, d as e}`, or a single
/// wildcard marker for `use a.b.*`.
auto record_use_bindings(const ast::use_decl &decl, file_id_type file_id,
                         program_index &index) -> void {
  auto &bindings = index.imports[file_id];

  if (!decl.instantiation_args.empty()) {
    // A functor instantiation (`use audited[postgres] as db`) is not an
    // ordinary import: the alias binds to the *instantiated* module, which the
    // checker materializes and registers. Skip it here so no binding pointing
    // at the bare functor is created (`check_functor_instantiation`).
    return;
  }

  if (!decl.selector.has_value()) {
    if (decl.path.empty()) {
      return;
    }
    bindings.push_back(import_binding{
        .local_name = decl.alias.value_or(decl.path.back()),
        .path = decl.path,
        .leaf_name = {},
        .is_wildcard = false,
        .span = decl.span,
    });
    return;
  }

  if (decl.selector->kind == ast::use_selector_kind::wildcard) {
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
/// traits, concepts, functions, static bindings, impls, extends), also
/// indexing a sum type's variants by name so a bare `@variant` can be found
/// later without knowing which sum type it belongs to.
auto record_module_item(const ast::node &item, file_id_type file_id,
                        module_members &members) -> void {
  switch (item.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = ast::cast<const ast::type_decl>(item);
    if (decl.name.empty()) {
      return;
    }
    members.types.emplace(decl.name,
                          type_decl_ref{.decl = &decl, .file_id = file_id});
    if (decl.definition != nullptr &&
        decl.definition->kind == ast::node_kind::sum_type_def) {
      const auto &sum =
          ast::cast<const ast::sum_type_def>(*decl.definition);
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
    const auto &decl = ast::cast<const ast::trait_decl>(item);
    if (!decl.name.empty()) {
      members.traits.emplace(decl.name,
                             trait_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = ast::cast<const ast::concept_decl>(item);
    if (!decl.name.empty()) {
      members.concepts.emplace(
          decl.name, concept_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::signature_decl: {
    const auto &decl = ast::cast<const ast::signature_decl>(item);
    if (!decl.name.empty()) {
      members.signatures.emplace(
          decl.name, signature_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::func_decl: {
    const auto &decl = ast::cast<const ast::func_decl>(item);
    if (!decl.name.empty()) {
      members.functions.emplace(
          decl.name, func_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::static_decl: {
    const auto &decl = ast::cast<const ast::static_decl>(item);
    if (decl.decl_kind == ast::static_decl_kind::binding &&
        !decl.name.empty()) {
      members.statics.emplace(
          decl.name, static_decl_ref{.decl = &decl, .file_id = file_id});
    }
    return;
  }
  case ast::node_kind::impl_decl: {
    const auto &decl = ast::cast<const ast::impl_decl>(item);
    members.impls.push_back(impl_ref{
        .decl = &decl, .module_name = members.module_name, .file_id = file_id});
    return;
  }
  case ast::node_kind::extend_decl: {
    const auto &decl = ast::cast<const ast::extend_decl>(item);
    members.extends.push_back(extend_ref{
        .decl = &decl, .module_name = members.module_name, .file_id = file_id});
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
      record_use_bindings(ast::cast<const ast::use_decl>(*item), file_id,
                          index);
      continue;
    }
    if (item->kind == ast::node_kind::sub_module_decl) {
      const auto &decl = ast::cast<const ast::sub_module_decl>(*item);
      if (decl.is_functor()) {
        // A parameterized module is a functor: recorded on its parent so
        // instantiation can find it, but *not* indexed as a module of its own
        // (its body is elaborated per instantiation, not once as written).
        if (!decl.name.empty()) {
          members.functors.emplace(
              decl.name, functor_decl_ref{.decl = &decl, .file_id = file_id});
        }
        continue;
      }
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

  // `use a.b as c` is recorded like a member selection (`a` + leaf `b`),
  // because while one file is being indexed it is not yet known whether
  // `a.b` is a module. Now that every module is: a selection naming a
  // module is a module import, bound under its (possibly renamed) local
  // name — exactly what `use a.b` would have bound, with the alias applied.
  for (auto &[file_id, bindings] : index.imports) {
    for (auto &binding : bindings) {
      if (binding.is_wildcard || binding.leaf_name.empty()) {
        continue;
      }
      auto module_path = binding.path;
      module_path.push_back(binding.leaf_name);
      if (index.find_module(join_strings(module_path, ".")) != nullptr) {
        binding.path = std::move(module_path);
        binding.leaf_name.clear();
      }
    }
  }

  return index;
}

} // namespace cinder::semantic
