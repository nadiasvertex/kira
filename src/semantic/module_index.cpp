#include "module_index.h"

#include <algorithm>
#include <ranges>

namespace kira::semantic {
namespace {

/// Builds one `module_file_record` per input file that has a valid `module`
/// declaration, deriving its parent module path from all but the last
/// dotted segment.
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
        .location =
            source_location{
                .file_id = input.file_id,
                .span = input.ast_file->module_decl->span,
            },
        .file_id = input.file_id,
        .ast_file = input.ast_file,
    });
  }

  return module_files;
}

/// Recursively records one `submodule_declaration_record` for every nested
/// `module Name` / `module Name: ...` item in `items`, tracking whether each
/// has an inline body (`is_inline`) and recursing into any that do.
auto collect_submodule_declarations(
    const std::vector<ast::ptr<ast::node>> &items,
    const std::vector<std::string> &parent_path, file_id_type file_id,
    std::vector<submodule_declaration_record> &out) -> void {
  for (const auto &item : items) {
    if (item == nullptr || item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(*item);
    if (decl.is_functor()) {
      // A parameterized module is not a module of its own in the graph; it is
      // instantiated per argument tuple. Skip it (and its body) here.
      continue;
    }
    auto module_path = parent_path;
    module_path.push_back(decl.name);
    out.push_back(submodule_declaration_record{
        .module_name = join_module_path_prefix(module_path, module_path.size()),
        .parent_module_name =
            join_module_path_prefix(parent_path, parent_path.size()),
        .child_name = decl.name,
        .location =
            source_location{
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

/// Recursively records one `use_decl_record` for every `use` declaration
/// found in `items`, at any nesting depth of inline submodules, tagging each
/// with the module path it was imported from.
auto collect_use_declarations(const std::vector<ast::ptr<ast::node>> &items,
                              const std::vector<std::string> &module_path,
                              file_id_type file_id,
                              std::vector<use_decl_record> &out) -> void {
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }

    if (item->kind == ast::node_kind::use_decl) {
      const auto &decl = dynamic_cast<const ast::use_decl &>(*item);
      out.push_back(use_decl_record{
          .decl = &decl,
          .importer_module_name =
              join_module_path_prefix(module_path, module_path.size()),
          .location =
              source_location{
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

    const auto &decl = dynamic_cast<const ast::sub_module_decl &>(*item);
    if (decl.is_functor()) {
      continue; // functor bodies are elaborated per instantiation
    }
    auto child_path = module_path;
    child_path.push_back(decl.name);
    collect_use_declarations(decl.items, child_path, file_id, out);
  }
}

} // namespace

/// Joins `parts` with `separator` between each element.
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

/// Strips the last dotted segment of `module_name`.
auto parent_module_name(std::string_view module_name) -> std::string {
  const auto separator = module_name.rfind('.');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string(module_name.substr(0, separator));
}

/// Returns the first dotted segment of `module_name`.
auto module_root_name(std::string_view module_name) -> std::string_view {
  const auto separator = module_name.find('.');
  if (separator == std::string_view::npos) {
    return module_name;
  }
  return module_name.substr(0, separator);
}

/// Appends `child` to `parent` as a new dotted segment.
auto append_module_name(std::string_view parent, std::string_view child)
    -> std::string {
  if (parent.empty()) {
    return std::string(child);
  }
  return std::format("{}.{}", parent, child);
}

/// A descendant must have `ancestor` as a full dotted prefix (not merely a
/// string prefix), so `geometry.shapes` is a descendant of `geometry` but not
/// of `geo`.
auto is_same_or_descendant_module(std::string_view module_name,
                                  std::string_view ancestor) -> bool {
  if (ancestor.empty()) {
    return false;
  }
  return module_name == ancestor || (module_name.starts_with(ancestor) &&
                                     module_name.size() > ancestor.size() &&
                                     module_name[ancestor.size()] == '.');
}

/// `def` (no explicit modifier) reports as "module" here, matching Kira's
/// default module-visibility rule.
auto visibility_name(ast::visibility visibility) -> std::string_view {
  switch (visibility) {
  case ast::visibility::def:
  case ast::visibility::module:
    return "module";
  case ast::visibility::pub:
    return "pub";
  case ast::visibility::file:
    return "file";
  }
  return "module";
}

/// Tailors the fix-it suggestion to the specific visibility that blocked an
/// import.
auto visibility_help(ast::visibility visibility, std::string_view parent_name)
    -> std::string {
  switch (visibility) {
  case ast::visibility::pub:
    return std::format(
        "Mark the module `pub` only when it should be importable outside `{}`.",
        parent_name);
  case ast::visibility::def:
  case ast::visibility::module:
    return std::format("Import this module from `{}` or one of its submodules, "
                       "or widen the declaration's visibility.",
                       parent_name);
  case ast::visibility::file:
    return "Keep the import in the declaring file, or widen the module "
           "declaration's visibility.";
  }
  return {};
}

/// Convenience wrapper joining `file.module_decl->path` with `.`, or
/// `nullopt` when the file has no usable module declaration.
auto module_path_key(const ast::file &file) -> std::optional<std::string> {
  if (file.module_decl == nullptr || file.module_decl->has_error ||
      file.module_decl->path.empty()) {
    return std::nullopt;
  }
  return join_strings(file.module_decl->path, ".");
}

/// Wraps `module_symbol_spec` into a full `semantic_symbol`-shaped record
/// with placeholder id/scope, for callers that only need the symbol's shape
/// (name, kind, visibility, location) before it has been interned.
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

/// Joins the first `limit` segments of `path` with `.`; returns an empty
/// string when `limit` is zero or `path` is empty.
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

/// Splits `module_name` on `.` into its segments.
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

    parts.emplace_back(
        module_name.substr(segment_start, separator - segment_start));
    segment_start = separator + 1;
  }
  return parts;
}

/// Builds the underlying session, then refreshes each module scope's cached
/// symbol list from the session's scope table so it reflects every symbol
/// added while walking (not just those seen before the scope was created).
auto build_semantic_resolution_index(const std::vector<parsed_module> &inputs)
    -> semantic_resolution_index {
  auto index =
      semantic_resolution_index{.session = build_semantic_session(inputs)};

  for (auto &module_scope : index.session.module_scopes) {
    const auto *scope = find_semantic_scope(index.session, module_scope.scope);
    if (scope == nullptr) {
      continue;
    }
    module_scope.symbols = scope->symbols;
  }

  return index;
}

/// Linear search over the session's module scopes for an exact name match.
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

/// Searches a module's symbols in reverse-declaration order so the most
/// recently added definition of a shadowed name wins.
auto find_module_scope_symbol(
    const semantic_resolution_index &index,
    std::string_view
        module_name, // NOLINT(bugprone-easily-swappable-parameters)
    std::string_view symbol_name) -> const semantic_symbol * {
  const auto *module_scope = find_module_scope(index, module_name);
  if (module_scope == nullptr) {
    return nullptr;
  }

  for (unsigned int it : std::views::reverse(module_scope->symbols)) {
    const auto *symbol = find_semantic_symbol(index.session, it);
    if (symbol != nullptr && symbol->name == symbol_name) {
      return symbol;
    }
  }
  return nullptr;
}

/// Collects module files, then recursively collects every nested submodule
/// declaration within each one.
auto build_module_session_index(const std::vector<parsed_module> &inputs)
    -> module_session_index {
  auto index = module_session_index{
      .module_files = collect_module_files(inputs),
      .submodule_declarations = {},
  };

  for (const auto &module_file : index.module_files) {
    if (module_file.ast_file == nullptr ||
        module_file.ast_file->module_decl == nullptr) {
      continue;
    }

    collect_submodule_declarations(
        module_file.ast_file->items, module_file.ast_file->module_decl->path,
        module_file.file_id, index.submodule_declarations);
  }

  return index;
}

/// Treats an out-of-range `file_id` as "no error" rather than indexing
/// out of bounds.
auto has_file_error(const std::vector<bool> &file_has_errors,
                    file_id_type file_id) -> bool {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return false;
  }
  return file_has_errors[file_id];
}

/// Linear search for the file record declaring exactly `module_name`.
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

/// Linear search for a submodule declaration matching both the declaring
/// file and the exact module path, used to detect conflicts between an
/// inline submodule and a same-named external file.
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

/// Linear search for any submodule declaration matching `module_name`,
/// regardless of which parent file declared it.
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

/// Whether any file's module path begins with `root_name` as its top-level
/// segment.
auto session_owns_root_module(const module_session_index &index,
                              std::string_view root_name) -> bool {
  return std::ranges::any_of(
      index.module_files, [&root_name](const auto &module) -> bool {
        return module_root_name(module.module_name) == root_name;
      });
}

/// A module is "contained" if it either has its own file or was declared
/// as a submodule somewhere in the session.
auto session_contains_module(const module_session_index &index,
                             std::string_view module_name) -> bool {
  return find_module_file(index.module_files, module_name) != nullptr ||
         find_submodule_declaration_by_name(index.submodule_declarations,
                                            module_name) != nullptr;
}

/// Walks up from `module_name` through successive parent paths until a
/// declared module file or submodule declaration is found.
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
    if (const auto *submodule = find_submodule_declaration_by_name(
            index.submodule_declarations, current)) {
      return module_anchor_record{
          .module_name = submodule->module_name,
          .location = submodule->location,
      };
    }
    current = parent_module_name(current);
  }
  return std::nullopt;
}

/// True if either the module's own file, or the file declaring it as a
/// submodule, already has a recorded error.
auto module_resolution_blocked_by_errors(
    const module_session_index &index, std::string_view module_name,
    const std::vector<bool> &file_has_errors) -> bool {
  if (const auto *module = find_module_file(index.module_files, module_name)) {
    if (has_file_error(file_has_errors, module->file_id)) {
      return true;
    }
  }

  if (const auto *submodule = find_submodule_declaration_by_name(
          index.submodule_declarations, module_name)) {
    if (has_file_error(file_has_errors, submodule->parent_file_id)) {
      return true;
    }
  }

  return false;
}

/// Implements Kira's visibility rules: `pub` is visible everywhere, the
/// default/`module` visibility is visible to the declaring module and its
/// descendants, and `file` only to the exact declaring file.
auto is_import_visible(const submodule_declaration_record &declaration,
                       std::string_view importer_module_name,
                       file_id_type importer_file_id) -> bool {
  switch (declaration.visibility) {
  case ast::visibility::pub:
    return true;
  case ast::visibility::def:
  case ast::visibility::module:
    return is_same_or_descendant_module(importer_module_name,
                                        declaration.parent_module_name);
  case ast::visibility::file:
    return importer_file_id == declaration.parent_file_id;
  }
  return false;
}

/// Skips files with no valid module declaration or an already-recorded
/// error, then recursively collects `use` declarations from the rest.
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
