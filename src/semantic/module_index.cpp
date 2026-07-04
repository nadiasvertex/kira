#include "module_index.h"

#include <format>

namespace kira::semantic {
namespace {

auto collect_module_scopes(const std::vector<ast::ptr<ast::node>> &items,
                           std::string_view module_name, file_id_type file_id,
                           std::vector<module_scope_record> &out) -> void {
  auto scope = module_scope_record{
      .module_name = std::string(module_name),
      .file_id = file_id,
      .scope = k_invalid_scope_id,
      .symbols = {},
  };

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (const auto symbol = module_scope_symbol(*item, file_id)) {
      scope.symbols.push_back(symbol->id);
    }
  }

  out.push_back(std::move(scope));

  for (const auto &item : items) {
    if (item == nullptr || item->has_error ||
        item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = static_cast<const ast::sub_module_decl &>(*item);
    if (decl.items.empty()) {
      continue;
    }

    const auto child_module_name = append_module_name(module_name, decl.name);
    collect_module_scopes(decl.items, child_module_name, file_id, out);
  }
}

auto collect_module_files(const std::vector<parsed_module> &inputs)
    -> std::vector<module_file_record> {
  auto module_files = std::vector<module_file_record>{};
  module_files.reserve(inputs.size());

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }

    const auto &path = input.ast_file->module_decl->path;
    module_files.push_back(module_file_record{
        .module_name = join_module_path_prefix(path, path.size()),
        .parent_module_name =
            path.size() > 1 ? join_module_path_prefix(path, path.size() - 1)
                            : std::string{},
        .child_name = path.back(),
        .location = source_location{
            .file_id = input.file_id,
            .span = input.ast_file->module_decl->span,
        },
        .file_id = input.file_id,
        .ast_file = input.ast_file,
    });
  }

  return module_files;
}

auto collect_submodule_declarations(
    const std::vector<ast::ptr<ast::node>> &items,
    const std::vector<std::string> &parent_path, file_id_type file_id,
    std::vector<submodule_declaration_record> &out) -> void {
  for (const auto &item : items) {
    if (item == nullptr || item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = static_cast<const ast::sub_module_decl &>(*item);
    auto module_path = parent_path;
    module_path.push_back(decl.name);
    out.push_back(submodule_declaration_record{
        .module_name = join_module_path_prefix(module_path, module_path.size()),
        .parent_module_name =
            join_module_path_prefix(parent_path, parent_path.size()),
        .child_name = decl.name,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
        .parent_file_id = file_id,
        .visibility = decl.visibility,
        .is_inline = !decl.items.empty(),
    });

    if (!decl.items.empty()) {
      collect_submodule_declarations(decl.items, module_path, file_id, out);
    }
  }
}

auto collect_use_declarations(const std::vector<ast::ptr<ast::node>> &items,
                              const std::vector<std::string> &module_path,
                              file_id_type file_id,
                              std::vector<use_decl_record> &out) -> void {
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }

    if (item->kind == ast::node_kind::use_decl) {
      const auto &decl = static_cast<const ast::use_decl &>(*item);
      out.push_back(use_decl_record{
          .decl = &decl,
          .importer_module_name =
              join_module_path_prefix(module_path, module_path.size()),
          .location = source_location{
              .file_id = file_id,
              .span = decl.span,
          },
          .file_id = file_id,
      });
      continue;
    }

    if (item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = static_cast<const ast::sub_module_decl &>(*item);
    auto child_path = module_path;
    child_path.push_back(decl.name);
    collect_use_declarations(decl.items, child_path, file_id, out);
  }
}

} // namespace

auto join_strings(const std::vector<std::string> &parts,
                  std::string_view separator) -> std::string {
  if (parts.empty()) {
    return {};
  }

  auto out = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    out += separator;
    out += parts[i];
  }
  return out;
}

auto parent_module_name(std::string_view module_name) -> std::string {
  const auto separator = module_name.rfind('.');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string(module_name.substr(0, separator));
}

auto module_root_name(std::string_view module_name) -> std::string_view {
  const auto separator = module_name.find('.');
  if (separator == std::string_view::npos) {
    return module_name;
  }
  return module_name.substr(0, separator);
}

auto append_module_name(std::string_view parent,
                        std::string_view child) -> std::string {
  if (parent.empty()) {
    return std::string(child);
  }
  return std::format("{}.{}", parent, child);
}

auto is_same_or_descendant_module(std::string_view module_name,
                                  std::string_view ancestor) -> bool {
  if (ancestor.empty()) {
    return false;
  }
  return module_name == ancestor ||
         (module_name.starts_with(ancestor) &&
          module_name.size() > ancestor.size() &&
          module_name[ancestor.size()] == '.');
}

auto visibility_name(ast::visibility visibility) -> std::string_view {
  switch (visibility) {
  case ast::visibility::def:
  case ast::visibility::internal:
    return "internal";
  case ast::visibility::pub:
    return "pub";
  case ast::visibility::super:
    return "super";
  case ast::visibility::priv:
    return "priv";
  }
  return "internal";
}

auto visibility_help(ast::visibility visibility,
                     std::string_view parent_name) -> std::string {
  switch (visibility) {
  case ast::visibility::pub:
    return std::format(
        "Mark the module `pub` only when it should be importable outside `{}`.",
        parent_name);
  case ast::visibility::def:
  case ast::visibility::internal:
    return std::format(
        "Import this module from `{}` or one of its submodules, or widen the declaration's visibility.",
        parent_name);
  case ast::visibility::super:
    return std::format(
        "Only the parent module `{}` can import this module; widen the declaration if other modules need it.",
        parent_name);
  case ast::visibility::priv:
    return "Keep the import in the declaring file, or widen the module declaration's visibility.";
  }
  return {};
}

auto module_path_key(const ast::file &file) -> std::optional<std::string> {
  if (file.module_decl == nullptr || file.module_decl->has_error ||
      file.module_decl->path.empty()) {
    return std::nullopt;
  }
  return join_strings(file.module_decl->path, ".");
}

auto module_scope_symbol(const ast::node &node, file_id_type file_id)
    -> std::optional<module_scope_symbol_record> {
  if (const auto spec = module_symbol_spec(node, file_id)) {
    return module_scope_symbol_record{
        .id = k_invalid_symbol_id,
        .name = spec->name,
        .kind = spec->kind,
        .name_space = spec->name_space,
        .kind_name = std::string(semantic_symbol_kind_name(spec->kind)),
        .visibility = spec->visibility,
        .location = spec->location,
        .defining_scope = k_invalid_scope_id,
    };
  }
  return std::nullopt;
}

auto join_module_path_prefix(const std::vector<std::string> &path, size_t limit)
    -> std::string {
  if (limit == 0 || path.empty()) {
    return {};
  }

  auto out = path.front();
  for (size_t i = 1; i < limit; ++i) {
    out += '.';
    out += path[i];
  }
  return out;
}

auto split_module_name(std::string_view module_name)
    -> std::vector<std::string> {
  auto parts = std::vector<std::string>{};
  if (module_name.empty()) {
    return parts;
  }

  size_t segment_start = 0;
  while (segment_start < module_name.size()) {
    const auto separator = module_name.find('.', segment_start);
    if (separator == std::string_view::npos) {
      parts.emplace_back(module_name.substr(segment_start));
      break;
    }

    parts.emplace_back(module_name.substr(segment_start, separator - segment_start));
    segment_start = separator + 1;
  }
  return parts;
}

auto build_semantic_resolution_index(const std::vector<parsed_module> &inputs)
    -> semantic_resolution_index {
  auto index = semantic_resolution_index{.session = build_semantic_session(inputs)};

  for (auto &module_scope : index.session.module_scopes) {
    const auto *scope = find_semantic_scope(index.session, module_scope.scope);
    if (scope == nullptr) {
      continue;
    }
    module_scope.symbols = scope->symbols;
  }

  return index;
}

auto find_module_scope(const semantic_resolution_index &index,
                       std::string_view module_name)
    -> const module_scope_record * {
  for (const auto &scope : index.session.module_scopes) {
    if (scope.module_name == module_name) {
      return &scope;
    }
  }
  return nullptr;
}

auto find_module_scope_symbol(const semantic_resolution_index &index,
                              std::string_view module_name,
                              std::string_view symbol_name)
    -> const semantic_symbol * {
  const auto *module_scope = find_module_scope(index, module_name);
  if (module_scope == nullptr) {
    return nullptr;
  }

  for (auto it = module_scope->symbols.rbegin(); it != module_scope->symbols.rend(); ++it) {
    const auto *symbol = find_semantic_symbol(index.session, *it);
    if (symbol != nullptr && symbol->name == symbol_name) {
      return symbol;
    }
  }
  return nullptr;
}

auto build_module_session_index(const std::vector<parsed_module> &inputs)
    -> module_session_index {
  auto index = module_session_index{
      .module_files = collect_module_files(inputs),
      .submodule_declarations = {},
  };

  for (const auto &module_file : index.module_files) {
    if (module_file.ast_file == nullptr || module_file.ast_file->module_decl == nullptr) {
      continue;
    }

    collect_submodule_declarations(module_file.ast_file->items,
                                   module_file.ast_file->module_decl->path,
                                   module_file.file_id,
                                   index.submodule_declarations);
  }

  return index;
}

auto has_file_error(const std::vector<bool> &file_has_errors,
                    file_id_type file_id) -> bool {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return false;
  }
  return file_has_errors[file_id];
}

auto find_module_file(const std::vector<module_file_record> &modules,
                      std::string_view module_name)
    -> const module_file_record * {
  for (const auto &module : modules) {
    if (module.module_name == module_name) {
      return &module;
    }
  }
  return nullptr;
}

auto find_submodule_declaration(
    const std::vector<submodule_declaration_record> &submodules,
    file_id_type parent_file_id, std::string_view module_name)
    -> const submodule_declaration_record * {
  for (const auto &submodule : submodules) {
    if (submodule.parent_file_id == parent_file_id &&
        submodule.module_name == module_name) {
      return &submodule;
    }
  }
  return nullptr;
}

auto find_submodule_declaration_by_name(
    const std::vector<submodule_declaration_record> &submodules,
    std::string_view module_name) -> const submodule_declaration_record * {
  for (const auto &submodule : submodules) {
    if (submodule.module_name == module_name) {
      return &submodule;
    }
  }
  return nullptr;
}

auto session_owns_root_module(const module_session_index &index,
                              std::string_view root_name) -> bool {
  for (const auto &module : index.module_files) {
    if (module_root_name(module.module_name) == root_name) {
      return true;
    }
  }
  return false;
}

auto session_contains_module(const module_session_index &index,
                             std::string_view module_name) -> bool {
  return find_module_file(index.module_files, module_name) != nullptr ||
         find_submodule_declaration_by_name(index.submodule_declarations,
                                            module_name) != nullptr;
}

auto find_nearest_module_anchor(const module_session_index &index,
                                std::string_view module_name)
    -> std::optional<module_anchor_record> {
  auto current = std::string(module_name);
  while (!current.empty()) {
    if (const auto *module = find_module_file(index.module_files, current)) {
      return module_anchor_record{
          .module_name = module->module_name,
          .location = module->location,
      };
    }
    if (const auto *submodule =
            find_submodule_declaration_by_name(index.submodule_declarations, current)) {
      return module_anchor_record{
          .module_name = submodule->module_name,
          .location = submodule->location,
      };
    }
    current = parent_module_name(current);
  }
  return std::nullopt;
}

auto module_resolution_blocked_by_errors(
    const module_session_index &index, std::string_view module_name,
    const std::vector<bool> &file_has_errors) -> bool {
  if (const auto *module = find_module_file(index.module_files, module_name)) {
    if (has_file_error(file_has_errors, module->file_id)) {
      return true;
    }
  }

  if (const auto *submodule =
          find_submodule_declaration_by_name(index.submodule_declarations,
                                             module_name)) {
    if (has_file_error(file_has_errors, submodule->parent_file_id)) {
      return true;
    }
  }

  return false;
}

auto is_import_visible(const submodule_declaration_record &declaration,
                       std::string_view importer_module_name,
                       file_id_type importer_file_id) -> bool {
  switch (declaration.visibility) {
  case ast::visibility::pub:
    return true;
  case ast::visibility::def:
  case ast::visibility::internal:
    return is_same_or_descendant_module(importer_module_name,
                                        declaration.parent_module_name);
  case ast::visibility::super:
    return importer_module_name == declaration.parent_module_name;
  case ast::visibility::priv:
    return importer_file_id == declaration.parent_file_id;
  }
  return false;
}

auto collect_use_decl_records(const std::vector<parsed_module> &inputs,
                              const std::vector<bool> &file_has_errors)
    -> std::vector<use_decl_record> {
  auto imports = std::vector<use_decl_record>{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }

    collect_use_declarations(input.ast_file->items,
                             input.ast_file->module_decl->path, input.file_id,
                             imports);
  }

  return imports;
}

} // namespace kira::semantic
