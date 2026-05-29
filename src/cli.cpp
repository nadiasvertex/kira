#include "cli.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "k-parser/parser.h"
#include "src/module_metadata.pb.h"

namespace kira {
namespace {

namespace fs = std::filesystem;

/// File extension used for serialized module metadata artifacts.
constexpr std::string_view kMetadataExtension = ".kmeta.pb";

/// Schema version embedded in every emitted module metadata payload.
constexpr uint32_t kModuleMetadataSchemaVersion = 1;

/// Append text to a diagnostic buffer, inserting a separating newline when needed.
///
/// @param buffer Destination buffer that accumulates rendered diagnostics.
/// @param text Text fragment to append.
auto append_text(std::string &buffer, std::string_view text) -> void {
  if (text.empty()) {
    return;
  }
  if (!buffer.empty() && buffer.back() != '\n') {
    buffer += '\n';
  }
  buffer += text;
}

/// Append one driver error line to the plain-text diagnostic buffer.
///
/// @param buffer Destination buffer that accumulates rendered diagnostics.
/// @param message Error message without the `error:` prefix.
auto append_error(std::string &buffer, std::string_view message) -> void {
  append_text(buffer, std::format("error: {}", message));
}

/// Convert a filesystem path to the normalized slash-separated form used in reports.
///
/// @param path Filesystem path to normalize.
[[nodiscard]] auto normalize_path(const fs::path &path) -> std::string {
  return path.lexically_normal().generic_string();
}

/// Join string segments with a caller-provided separator.
///
/// @param parts Ordered string segments to join.
/// @param separator Separator inserted between adjacent segments.
[[nodiscard]] auto join_strings(const std::vector<std::string> &parts,
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

/// Choose a stable fallback stem for metadata when a source file has no filename stem.
///
/// @param source_path Source file path being compiled.
[[nodiscard]] auto source_stem_or_default(const fs::path &source_path)
    -> std::string {
  auto stem = source_path.stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return "module";
}

/// Render a human-readable module label for compile summaries.
///
/// @param module Compiled module record to describe.
[[nodiscard]] auto module_display_name(const compiled_module &module) -> std::string {
  if (!module.module_path.empty()) {
    return join_strings(module.module_path, ".");
  }
  return module.source_path;
}

/// Return the parent module portion of a dotted module path.
///
/// @param module_name Canonical dotted module name.
[[nodiscard]] auto parent_module_name(std::string_view module_name) -> std::string {
  const auto separator = module_name.rfind('.');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string(module_name.substr(0, separator));
}

/// Return the root segment of a dotted module path.
///
/// @param module_name Canonical dotted module name.
[[nodiscard]] auto module_root_name(std::string_view module_name) -> std::string_view {
  const auto separator = module_name.find('.');
  if (separator == std::string_view::npos) {
    return module_name;
  }
  return module_name.substr(0, separator);
}

/// Extend a dotted module path with one child module name.
///
/// @param parent Existing parent module path.
/// @param child Child module name to append.
[[nodiscard]] auto append_module_name(std::string_view parent,
                                      std::string_view child) -> std::string {
  if (parent.empty()) {
    return std::string(child);
  }
  return std::format("{}.{}", parent, child);
}

/// Report whether `module_name` is the same module as `ancestor` or one of its descendants.
///
/// @param module_name Candidate descendant module path.
/// @param ancestor Candidate ancestor module path.
[[nodiscard]] auto is_same_or_descendant_module(std::string_view module_name,
                                                std::string_view ancestor) -> bool {
  if (ancestor.empty()) {
    return false;
  }
  return module_name == ancestor ||
         (module_name.starts_with(ancestor) && module_name.size() > ancestor.size() &&
          module_name[ancestor.size()] == '.');
}

/// Render user-facing visibility text for diagnostics.
///
/// @param visibility Visibility modifier attached to a declaration.
[[nodiscard]] auto visibility_name(ast::visibility visibility) -> std::string_view {
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

/// Explain how to make an import valid after a visibility failure.
///
/// @param visibility Visibility modifier attached to the imported declaration.
/// @param parent_name Parent module that owns the imported child module.
[[nodiscard]] auto visibility_help(ast::visibility visibility,
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

/// Read an entire source file into memory for parsing.
///
/// @param path Source file path to load.
[[nodiscard]] auto read_source_file(const fs::path &path)
    -> std::expected<std::string, std::string> {
  auto in = std::ifstream(path, std::ios::binary);
  if (!in) {
    return std::unexpected{
        std::format("failed to open `{}`", normalize_path(path))};
  }

  auto buffer = std::ostringstream{};
  buffer << in.rdbuf();
  if (!in.good() && !in.eof()) {
    return std::unexpected{
        std::format("failed while reading `{}`", normalize_path(path))};
  }

  return buffer.str();
}

/// Map AST visibility values into the persisted metadata enum.
///
/// @param visibility AST visibility value to encode.
[[nodiscard]] auto visibility_to_proto(ast::visibility visibility)
    -> metadata::v1::ModuleVisibility {
  switch (visibility) {
  case ast::visibility::def:
    return metadata::v1::MODULE_VISIBILITY_DEFAULT;
  case ast::visibility::pub:
    return metadata::v1::MODULE_VISIBILITY_PUBLIC;
  case ast::visibility::internal:
    return metadata::v1::MODULE_VISIBILITY_INTERNAL;
  case ast::visibility::super:
    return metadata::v1::MODULE_VISIBILITY_SUPER;
  case ast::visibility::priv:
    return metadata::v1::MODULE_VISIBILITY_PRIVATE;
  }
  return metadata::v1::MODULE_VISIBILITY_UNSPECIFIED;
}

/// Map parsed `use` selector kinds into the persisted metadata enum.
///
/// @param kind AST selector kind to encode.
[[nodiscard]] auto selector_kind_to_proto(ast::UseSelectorKind kind)
    -> metadata::v1::ImportSelectorKind {
  switch (kind) {
  case ast::UseSelectorKind::Single:
    return metadata::v1::IMPORT_SELECTOR_KIND_SINGLE;
  case ast::UseSelectorKind::Group:
    return metadata::v1::IMPORT_SELECTOR_KIND_GROUP;
  case ast::UseSelectorKind::Wildcard:
    return metadata::v1::IMPORT_SELECTOR_KIND_WILDCARD;
  }
  return metadata::v1::IMPORT_SELECTOR_KIND_UNSPECIFIED;
}

/// Map top-level AST node kinds into the persisted metadata enum.
///
/// @param kind AST node kind to encode.
[[nodiscard]] auto symbol_kind_to_proto(ast::node_kind kind)
    -> metadata::v1::TopLevelSymbolKind {
  switch (kind) {
  case ast::node_kind::use_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_USE;
  case ast::node_kind::type_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_TYPE;
  case ast::node_kind::trait_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_TRAIT;
  case ast::node_kind::impl_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_IMPL;
  case ast::node_kind::concept_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_CONCEPT;
  case ast::node_kind::func_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_FUNCTION;
  case ast::node_kind::sub_module_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_SUBMODULE;
  case ast::node_kind::dep_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_DEP;
  case ast::node_kind::static_decl:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_STATIC;
  case ast::node_kind::splice_stmt:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_SPLICE;
  case ast::node_kind::error_node:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_ERROR;
  default:
    return metadata::v1::TOP_LEVEL_SYMBOL_KIND_UNSPECIFIED;
  }
}

/// Choose a stable display label for metadata emitted from `static` declarations.
///
/// @param decl Static declaration to describe.
[[nodiscard]] auto static_decl_label(const ast::static_decl &decl) -> std::string {
  if (!decl.name.empty()) {
    return decl.name;
  }

  switch (decl.decl_kind) {
  case ast::static_decl_kind::conditional_compilation:
    return "static if";
  case ast::static_decl_kind::binding:
    return "static";
  case ast::static_decl_kind::assertion:
    return "static assert";
  case ast::static_decl_kind::for_inline:
  case ast::static_decl_kind::for_block:
    return "static for";
  }
  return "static";
}

/// Extract the effective top-level visibility from a parsed item node.
///
/// @param node Top-level AST item.
[[nodiscard]] auto top_level_visibility(const ast::node &node) -> ast::visibility {
  switch (node.kind) {
  case ast::node_kind::use_decl:
    return static_cast<const ast::use_decl &>(node).visibility;
  case ast::node_kind::type_decl:
    return static_cast<const ast::type_decl &>(node).visibility;
  case ast::node_kind::trait_decl:
    return static_cast<const ast::trait_decl &>(node).visibility;
  case ast::node_kind::concept_decl:
    return static_cast<const ast::concept_decl &>(node).visibility;
  case ast::node_kind::func_decl:
    return static_cast<const ast::func_decl &>(node).visibility;
  case ast::node_kind::sub_module_decl:
    return static_cast<const ast::sub_module_decl &>(node).visibility;
  case ast::node_kind::static_decl:
    return static_cast<const ast::static_decl &>(node).visibility;
  default:
    return ast::visibility::def;
  }
}

/// Extract the user-facing symbol name for a parsed top-level item.
///
/// @param node Top-level AST item.
[[nodiscard]] auto top_level_name(const ast::node &node) -> std::string {
  switch (node.kind) {
  case ast::node_kind::use_decl:
    return join_strings(static_cast<const ast::use_decl &>(node).path, ".");
  case ast::node_kind::type_decl:
    return static_cast<const ast::type_decl &>(node).name;
  case ast::node_kind::trait_decl:
    return static_cast<const ast::trait_decl &>(node).name;
  case ast::node_kind::concept_decl:
    return static_cast<const ast::concept_decl &>(node).name;
  case ast::node_kind::func_decl:
    return static_cast<const ast::func_decl &>(node).name;
  case ast::node_kind::sub_module_decl:
    return static_cast<const ast::sub_module_decl &>(node).name;
  case ast::node_kind::dep_decl:
    return static_cast<const ast::dep_decl &>(node).name;
  case ast::node_kind::static_decl:
    return static_decl_label(static_cast<const ast::static_decl &>(node));
  case ast::node_kind::error_node:
    return static_cast<const ast::error_node &>(node).description;
  default:
    return {};
  }
}

/// Remove surrounding quotes from dependency string literals before persisting them.
///
/// @param value Parsed dependency field value.
[[nodiscard]] auto unquote_string_literal(std::string_view value) -> std::string {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return std::string(value.substr(1, value.size() - 2));
  }
  return std::string(value);
}

/// Append one import entry to a module metadata record.
///
/// @param decl Parsed `use` declaration to encode.
/// @param metadata Metadata message being populated.
auto add_import_metadata(const ast::use_decl &decl,
                         metadata::v1::ModuleMetadata &metadata) -> void {
  auto *import = metadata.add_imports();
  for (const auto &part : decl.path) {
    import->add_path(part);
  }

  if (!decl.selector.has_value()) {
    import->set_selector_kind(metadata::v1::IMPORT_SELECTOR_KIND_UNSPECIFIED);
    return;
  }

  import->set_selector_kind(selector_kind_to_proto(decl.selector->kind));
  for (const auto &item : decl.selector->items) {
    auto *metadata_item = import->add_items();
    metadata_item->set_name(item.name);
    if (item.alias.has_value()) {
      metadata_item->set_alias(*item.alias);
    }
  }
}

/// Append one dependency entry to a module metadata record.
///
/// @param decl Parsed `dep` declaration to encode.
/// @param metadata Metadata message being populated.
auto add_dependency_metadata(const ast::dep_decl &decl,
                             metadata::v1::ModuleMetadata &metadata) -> void {
  auto *dependency = metadata.add_dependencies();
  dependency->set_name(decl.name);
  for (const auto &field : decl.fields) {
    (*dependency->mutable_fields())[field.key] = unquote_string_literal(field.value);
  }
}

/// Append one top-level symbol summary to a module metadata record.
///
/// @param node Parsed top-level item to encode.
/// @param metadata Metadata message being populated.
auto add_symbol_metadata(const ast::node &node,
                         metadata::v1::ModuleMetadata &metadata) -> void {
  auto *symbol = metadata.add_top_level_symbols();
  symbol->set_name(top_level_name(node));
  symbol->set_kind(symbol_kind_to_proto(node.kind));
  symbol->set_visibility(visibility_to_proto(top_level_visibility(node)));
  symbol->set_has_parse_error(node.has_error);
}

/// Build the serialized metadata payload for one parsed source file.
///
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
[[nodiscard]] auto build_module_metadata(const ast::file &file,
                                         const fs::path &source_path)
    -> metadata::v1::ModuleMetadata {
  auto metadata = metadata::v1::ModuleMetadata{};
  metadata.set_schema_version(kModuleMetadataSchemaVersion);
  metadata.set_source_path(normalize_path(source_path));
  metadata.set_no_prelude(file.no_prelude);
  metadata.set_parser_error_count(0);

  if (file.module_decl != nullptr) {
    for (const auto &part : file.module_decl->path) {
      metadata.add_module_path(part);
    }
  }

  for (const auto &item : file.items) {
    if (item == nullptr) {
      continue;
    }

    add_symbol_metadata(*item, metadata);

    switch (item->kind) {
    case ast::node_kind::use_decl:
      add_import_metadata(static_cast<const ast::use_decl &>(*item), metadata);
      break;
    case ast::node_kind::dep_decl:
      add_dependency_metadata(static_cast<const ast::dep_decl &>(*item), metadata);
      break;
    default:
      break;
    }
  }

  return metadata;
}

/// Compute the on-disk metadata path for one compiled module.
///
/// @param metadata_root Root directory configured for metadata output.
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
[[nodiscard]] auto metadata_output_path(const fs::path &metadata_root,
                                        const ast::file &file,
                                        const fs::path &source_path) -> fs::path {
  auto relative = fs::path{};

  if (file.module_decl != nullptr && !file.module_decl->path.empty()) {
    for (size_t i = 0; i + 1 < file.module_decl->path.size(); ++i) {
      relative /= file.module_decl->path[i];
    }
    relative /= file.module_decl->path.back() + std::string(kMetadataExtension);
    return metadata_root / relative;
  }

  relative /= source_stem_or_default(source_path) + std::string(kMetadataExtension);
  return metadata_root / relative;
}

/// Serialize one module's metadata file and return its output path.
///
/// @param metadata_root Root directory configured for metadata output.
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
/// @param diagnostics Plain-text driver diagnostics buffer for I/O failures.
[[nodiscard]] auto write_module_metadata(const fs::path &metadata_root,
                                         const ast::file &file,
                                         const fs::path &source_path,
                                         std::string &diagnostics)
    -> std::expected<std::string, std::monostate> {
  auto output_path = metadata_output_path(metadata_root, file, source_path);
  auto output_parent = output_path.parent_path();

  auto ec = std::error_code{};
  if (!output_parent.empty()) {
    fs::create_directories(output_parent, ec);
    if (ec) {
      append_error(diagnostics,
                   std::format("failed to create `{}`: {}",
                               normalize_path(output_parent), ec.message()));
      return std::unexpected(std::monostate{});
    }
  }

  auto out = std::ofstream(output_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    append_error(diagnostics,
                 std::format("failed to open `{}` for writing",
                             normalize_path(output_path)));
    return std::unexpected(std::monostate{});
  }

  auto metadata = build_module_metadata(file, source_path);
  if (!metadata.SerializeToOstream(&out) || !out.good()) {
    out.close();
    fs::remove(output_path, ec);
    append_error(diagnostics,
                 std::format("failed to serialize module metadata to `{}`",
                             normalize_path(output_path)));
    return std::unexpected(std::monostate{});
  }

  return normalize_path(output_path);
}

/// Parsed source file plus the bookkeeping needed by later driver passes.
struct parsed_input {
  fs::path source_path;         ///< Original source file path passed to the CLI.
  file_id_type file_id = 0;     ///< Source manager file identifier for diagnostics.
  ast::ptr<ast::file> ast_file; ///< Parsed AST for the source file.
};

/// One source file that declares a canonical module path for the session.
struct module_file_record {
  std::string module_name;         ///< Fully-qualified module path declared by the file.
  std::string parent_module_name;  ///< Parent module path, if any.
  std::string child_name;          ///< Final segment within the parent module.
  source_location location;        ///< Location of the file-level module declaration.
  file_id_type file_id = 0;        ///< Source manager file identifier for the module file.
  const ast::file *ast_file = nullptr; ///< Borrowed pointer to the parsed file AST.
};

/// One nested `module` declaration gathered from within a parent module file.
struct submodule_declaration_record {
  std::string module_name;         ///< Fully-qualified child module path.
  std::string parent_module_name;  ///< Fully-qualified parent module path.
  std::string child_name;          ///< Final segment declared by the parent module.
  source_location location;        ///< Location of the nested `module` declaration.
  file_id_type parent_file_id = 0; ///< File that owns the nested declaration.
  ast::visibility visibility = ast::visibility::def; ///< Import visibility of the child.
  bool is_inline = false;          ///< True when the child module body is defined inline.
};

/// Session-wide module graph facts derived from the currently loaded inputs.
struct module_session_index {
  std::vector<module_file_record> module_files; ///< Source files that define module paths.
  std::vector<submodule_declaration_record>
      submodule_declarations; ///< Nested module declarations inside those files.
};

/// Anchor declaration used to build cross-file import diagnostics.
struct module_anchor_record {
  std::string module_name;  ///< Fully-qualified module path at the anchor site.
  source_location location; ///< Location of the anchor declaration.
};

/// One `use` declaration collected from the session, including inline submodules.
struct use_decl_record {
  const ast::use_decl *decl = nullptr; ///< Borrowed pointer to the parsed `use` node.
  std::string importer_module_name;    ///< Module containing the `use` declaration.
  source_location location;            ///< Location of the `use` declaration itself.
  file_id_type file_id = 0;            ///< Source manager file identifier for diagnostics.
};

/// Compute the canonical dotted module name for a parsed file declaration.
///
/// @param file Parsed file whose module declaration should be inspected.
[[nodiscard]] auto module_path_key(const ast::file &file)
    -> std::optional<std::string> {
  if (file.module_decl == nullptr || file.module_decl->has_error ||
      file.module_decl->path.empty()) {
    return std::nullopt;
  }
  return join_strings(file.module_decl->path, ".");
}

/// Join the first `limit` components of a module path with dots.
///
/// @param path Parsed module path components.
/// @param limit Number of leading components to join.
[[nodiscard]] auto join_module_path_prefix(const std::vector<std::string> &path,
                                           size_t limit) -> std::string {
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

/// Collect all file-level module declarations from the current input set.
///
/// @param inputs Parsed source files in the current compilation session.
[[nodiscard]] auto collect_module_files(const std::vector<parsed_input> &inputs)
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
        .ast_file = input.ast_file.get(),
    });
  }

  return module_files;
}

/// Recursively collect nested `module` declarations from one parent module body.
///
/// @param items Items declared directly inside the current module body.
/// @param parent_path Fully-qualified path of the current parent module.
/// @param file_id Source manager file identifier for the parent file.
/// @param out Destination vector for collected submodule declarations.
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
        .parent_module_name = join_module_path_prefix(parent_path, parent_path.size()),
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

/// Recursively collect `use` declarations from a module body and its inline children.
///
/// @param items Items declared directly inside the current module body.
/// @param module_path Fully-qualified path of the current module.
/// @param file_id Source manager file identifier for the enclosing file.
/// @param out Destination vector for collected `use` declarations.
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
          .importer_module_name = join_module_path_prefix(module_path, module_path.size()),
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

/// Build the reusable session index consumed by cross-file validation passes.
///
/// @param inputs Parsed source files in the current compilation session.
[[nodiscard]] auto build_module_session_index(const std::vector<parsed_input> &inputs)
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

/// Report whether a file has already been marked as failing earlier validation.
///
/// @param file_has_errors Per-file error flags.
/// @param file_id Source manager file identifier to query.
[[nodiscard]] auto has_file_error(const std::vector<bool> &file_has_errors,
                                  file_id_type file_id) -> bool {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return false;
  }
  return file_has_errors[file_id];
}

/// Look up a file-level module declaration by its canonical dotted module name.
///
/// @param modules File-level module declarations in the current session.
/// @param module_name Canonical dotted module name to find.
[[nodiscard]] auto find_module_file(const std::vector<module_file_record> &modules,
                                    std::string_view module_name)
    -> const module_file_record * {
  for (const auto &module : modules) {
    if (module.module_name == module_name) {
      return &module;
    }
  }
  return nullptr;
}

/// Look up a nested child-module declaration under one specific parent file.
///
/// @param submodules Nested child-module declarations in the current session.
/// @param parent_file_id Source manager file identifier for the parent file.
/// @param module_name Canonical dotted child module name to find.
[[nodiscard]] auto find_submodule_declaration(
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

/// Look up any nested child-module declaration by its canonical dotted name.
///
/// @param submodules Nested child-module declarations in the current session.
/// @param module_name Canonical dotted child module name to find.
[[nodiscard]] auto find_submodule_declaration_by_name(
    const std::vector<submodule_declaration_record> &submodules,
    std::string_view module_name) -> const submodule_declaration_record * {
  for (const auto &submodule : submodules) {
    if (submodule.module_name == module_name) {
      return &submodule;
    }
  }
  return nullptr;
}

/// Report whether the current session owns the imported root module namespace.
///
/// @param index Session-wide module graph facts.
/// @param root_name Root module name to test.
[[nodiscard]] auto session_owns_root_module(const module_session_index &index,
                                            std::string_view root_name) -> bool {
  for (const auto &module : index.module_files) {
    if (module_root_name(module.module_name) == root_name) {
      return true;
    }
  }
  return false;
}

/// Report whether a module path exists anywhere in the current session graph.
///
/// @param index Session-wide module graph facts.
/// @param module_name Canonical dotted module name to test.
[[nodiscard]] auto session_contains_module(const module_session_index &index,
                                           std::string_view module_name) -> bool {
  return find_module_file(index.module_files, module_name) != nullptr ||
         find_submodule_declaration_by_name(index.submodule_declarations,
                                            module_name) != nullptr;
}

/// Return a nearby declaration site that explains why a module path is owned in-session.
///
/// @param index Session-wide module graph facts.
/// @param module_name Canonical dotted module name being resolved.
[[nodiscard]] auto find_nearest_module_anchor(const module_session_index &index,
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

/// Report whether a target module is temporarily unusable because its defining file failed.
///
/// @param index Session-wide module graph facts.
/// @param module_name Canonical dotted module name being resolved.
/// @param file_has_errors Per-file error flags.
[[nodiscard]] auto module_resolution_blocked_by_errors(
    const module_session_index &index, std::string_view module_name,
    const std::vector<bool> &file_has_errors) -> bool {
  if (const auto *module = find_module_file(index.module_files, module_name)) {
    if (has_file_error(file_has_errors, module->file_id)) {
      return true;
    }
  }

  if (const auto *submodule =
          find_submodule_declaration_by_name(index.submodule_declarations, module_name)) {
    if (has_file_error(file_has_errors, submodule->parent_file_id)) {
      return true;
    }
  }

  return false;
}

/// Report whether an importer is allowed to reference a nested child module.
///
/// @param declaration Nested child-module declaration being imported.
/// @param importer_module_name Module that contains the `use` declaration.
/// @param importer_file_id Source manager file identifier for the importer.
[[nodiscard]] auto is_import_visible(
    const submodule_declaration_record &declaration,
    std::string_view importer_module_name, file_id_type importer_file_id) -> bool {
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

/// Collect all `use` declarations from currently valid source files.
///
/// @param inputs Parsed source files in the current compilation session.
/// @param file_has_errors Per-file error flags used to skip already-broken files.
[[nodiscard]] auto collect_use_decl_records(const std::vector<parsed_input> &inputs,
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

    collect_use_declarations(input.ast_file->items, input.ast_file->module_decl->path,
                             input.file_id, imports);
  }

  return imports;
}

/// Mark one source file as failing so later driver phases can skip it.
///
/// @param file_has_errors Per-file error flags.
/// @param file_id Source manager file identifier to mark.
auto mark_file_has_error(std::vector<bool> &file_has_errors,
                         file_id_type file_id) -> void;

/// Emit one unresolved-import diagnostic for a module path owned by this session.
///
/// @param index Session-wide module graph facts.
/// @param import_record `use` declaration currently being validated.
/// @param imported_module_name Canonical dotted module path that failed to resolve.
/// @param import_span Span to highlight inside the importer.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_unresolved_import(const module_session_index &index,
                            const use_decl_record &import_record,
                            std::string_view imported_module_name,
                            source_span import_span, diagnostic_bag &diag,
                            std::vector<bool> &file_has_errors) -> void {
  auto unresolved = diagnostic(
      diagnostic_level::error,
      std::format("import `{}` does not resolve in this compilation session",
                  imported_module_name),
      import_record.file_id);
  unresolved.with_label(import_span, "unresolved in-session import");

  if (const auto anchor = find_nearest_module_anchor(index, imported_module_name)) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("session-owned module `{}` is declared here",
                               anchor->module_name),
                   anchor->location.file_id)
            .with_label(anchor->location.span, "nearest declared module"));
  }

  unresolved.with_help(std::format(
      "Load module `{}` into this compilation session, or change the import to reference an external module namespace.",
      imported_module_name));
  diag.emit(std::move(unresolved));
  mark_file_has_error(file_has_errors, import_record.file_id);
}

/// Emit one visibility diagnostic for an in-session import that crosses module boundaries.
///
/// @param declaration Nested child-module declaration that rejected the import.
/// @param import_record `use` declaration currently being validated.
/// @param imported_module_name Canonical dotted module path being imported.
/// @param import_span Span to highlight inside the importer.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_inaccessible_import(const submodule_declaration_record &declaration,
                              const use_decl_record &import_record,
                              std::string_view imported_module_name,
                              source_span import_span, diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  auto inaccessible = diagnostic(
      diagnostic_level::error,
      std::format("module `{}` is not visible from module `{}`",
                  imported_module_name, import_record.importer_module_name),
      import_record.file_id);
  inaccessible.with_label(import_span, "imported here");
  inaccessible.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("module `{}` is declared here with `{}` visibility",
                             imported_module_name,
                             visibility_name(declaration.visibility)),
                 declaration.location.file_id)
          .with_label(declaration.location.span, "restricted module declaration"));
  inaccessible.with_help(
      visibility_help(declaration.visibility, declaration.parent_module_name));
  diag.emit(std::move(inaccessible));
  mark_file_has_error(file_has_errors, import_record.file_id);
}

/// Resolve one in-session module reference used by a parsed `use` declaration.
///
/// @param index Session-wide module graph facts.
/// @param import_record `use` declaration currently being validated.
/// @param imported_module_name Canonical dotted module path being imported.
/// @param import_span Span to highlight if the import fails.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
[[nodiscard]] auto validate_import_target(const module_session_index &index,
                                          const use_decl_record &import_record,
                                          std::string_view imported_module_name,
                                          source_span import_span,
                                          diagnostic_bag &diag,
                                          std::vector<bool> &file_has_errors)
    -> bool {
  if (!session_contains_module(index, imported_module_name)) {
    emit_unresolved_import(index, import_record, imported_module_name, import_span,
                           diag, file_has_errors);
    return false;
  }

  if (module_resolution_blocked_by_errors(index, imported_module_name,
                                          file_has_errors)) {
    return false;
  }

  const auto *declaration =
      find_submodule_declaration_by_name(index.submodule_declarations,
                                         imported_module_name);
  if (declaration == nullptr) {
    return true;
  }

  if (has_file_error(file_has_errors, declaration->parent_file_id)) {
    return false;
  }

  if (!is_import_visible(*declaration, import_record.importer_module_name,
                         import_record.file_id)) {
    emit_inaccessible_import(*declaration, import_record, imported_module_name,
                             import_span, diag, file_has_errors);
    return false;
  }

  return true;
}

/// Mark one source file as failing so later driver phases can skip it.
///
/// @param file_has_errors Per-file error flags.
/// @param file_id Source manager file identifier to mark.
auto mark_file_has_error(std::vector<bool> &file_has_errors,
                         file_id_type file_id) -> void {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return;
  }
  file_has_errors[file_id] = true;
}

/// Emit diagnostics when two source files claim the same module path.
///
/// @param inputs Parsed source files in the current compilation session.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto detect_duplicate_module_paths(const std::vector<parsed_input> &inputs,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void {
  /// Previously-seen module declaration used to diagnose duplicates.
  struct seen_module {
    std::string module_name;   ///< Canonical module path already claimed in this session.
    source_location location;  ///< Location of the first declaration for that module path.
  };

  auto seen_modules = std::vector<seen_module>{};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr) {
      continue;
    }

    auto module_name = module_path_key(*input.ast_file);
    if (!module_name.has_value()) {
      continue;
    }

    auto current_location = source_location{
        .file_id = input.file_id,
        .span = input.ast_file->module_decl->span,
    };

    const seen_module *previous = nullptr;
    for (const auto &candidate : seen_modules) {
      if (candidate.module_name == *module_name) {
        previous = &candidate;
        break;
      }
    }

    if (previous != nullptr) {
      auto duplicate = diagnostic(
          diagnostic_level::error,
          std::format("duplicate module path `{}`", *module_name),
          current_location.file_id);
      duplicate.with_label(current_location.span, "duplicate module declaration");
      duplicate.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("module `{}` was first declared here",
                                 *module_name),
                     previous->location.file_id)
              .with_label(previous->location.span,
                          "previous module declaration"));
      duplicate.with_help(
          "Give each source file a unique module path before compiling them "
          "together.");
      diag.emit(std::move(duplicate));

      mark_file_has_error(file_has_errors, current_location.file_id);
      mark_file_has_error(file_has_errors, previous->location.file_id);
      continue;
    }

    seen_modules.push_back(seen_module{
        .module_name = *module_name,
        .location = current_location,
    });
  }
}

/// Validate that separate module files agree with parent-module declarations.
///
/// @param index Session-wide module graph facts.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto validate_module_boundaries(const module_session_index &index,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void {
  for (const auto &module_file : index.module_files) {
    if (module_file.parent_module_name.empty() ||
        has_file_error(file_has_errors, module_file.file_id)) {
      continue;
    }

    const auto *parent =
        find_module_file(index.module_files, module_file.parent_module_name);
    if (parent == nullptr || has_file_error(file_has_errors, parent->file_id)) {
      continue;
    }

    const auto *declaration = find_submodule_declaration(
        index.submodule_declarations, parent->file_id, module_file.module_name);
    if (declaration == nullptr) {
      auto missing = diagnostic(
          diagnostic_level::error,
          std::format("module `{}` is not declared by parent module `{}`",
                      module_file.module_name, parent->module_name),
          module_file.file_id);
      missing.with_label(module_file.location.span, "child module defined here");
      missing.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("parent module `{}` is declared here",
                                 parent->module_name),
                     parent->file_id)
              .with_label(parent->location.span, "parent module declaration"));
      missing.with_help(std::format(
          "Add `module {}` to `{}`, or stop compiling `{}` as a separate "
          "module.",
          module_file.child_name, parent->module_name, module_file.module_name));
      diag.emit(std::move(missing));

      mark_file_has_error(file_has_errors, module_file.file_id);
      mark_file_has_error(file_has_errors, parent->file_id);
      continue;
    }

    if (!declaration->is_inline) {
      continue;
    }

    auto conflict = diagnostic(
        diagnostic_level::error,
        std::format(
            "module `{}` is declared inline and cannot also be defined in a "
            "separate file",
            module_file.module_name),
        module_file.file_id);
    conflict.with_label(module_file.location.span,
                        "separate file module declaration");
    conflict.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("inline submodule `{}` is declared here",
                               module_file.child_name),
                   declaration->location.file_id)
            .with_label(declaration->location.span,
                        "inline submodule declaration"));
    conflict.with_help(std::format(
        "Keep `module {}:` inline in `{}` or move it to its own file, but "
        "not both.",
        module_file.child_name, parent->module_name));
    diag.emit(std::move(conflict));

    mark_file_has_error(file_has_errors, module_file.file_id);
    mark_file_has_error(file_has_errors, declaration->location.file_id);
  }
}

/// Resolve `use` declarations against modules that belong to the current session.
///
/// @param inputs Parsed source files in the current compilation session.
/// @param index Session-wide module graph facts.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto validate_session_imports(const std::vector<parsed_input> &inputs,
                              const module_session_index &index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  const auto imports = collect_use_decl_records(inputs, file_has_errors);

  for (const auto &import_record : imports) {
    if (import_record.decl == nullptr || import_record.decl->has_error ||
        import_record.decl->path.empty() ||
        has_file_error(file_has_errors, import_record.file_id)) {
      continue;
    }

    const auto &decl = *import_record.decl;
    if (!session_owns_root_module(index, decl.path.front())) {
      continue;
    }

    if (!decl.selector.has_value()) {
      const auto imported_module_name = join_strings(decl.path, ".");
      validate_import_target(index, import_record, imported_module_name, decl.span,
                             diag, file_has_errors);
      continue;
    }

    const auto base_module_name = join_strings(decl.path, ".");
    if (!validate_import_target(index, import_record, base_module_name,
                                decl.selector->span, diag, file_has_errors)) {
      continue;
    }

    if (decl.selector->kind == ast::UseSelectorKind::Wildcard) {
      continue;
    }

    for (const auto &item : decl.selector->items) {
      const auto imported_module_name = append_module_name(base_module_name, item.name);
      if (!validate_import_target(index, import_record, imported_module_name,
                                  item.span, diag, file_has_errors)) {
        break;
      }
    }
  }
}

} // namespace

/// Parse CLI arguments into the compile driver's configuration structure.
///
/// @param argc Argument count passed to `main`.
/// @param argv Argument vector passed to `main`.
auto parse_args(int argc, char *argv[]) -> std::expected<cli_config, std::string> {
  if (argc < 1) {
    return std::unexpected{"argc must be at least 1"};
  }

  auto cfg = cli_config{
      .program_name = argv[0],
      .sources = {},
      .metadata_dir = std::string(kDefaultMetadataDir),
      .show_help = false,
  };

  bool parse_options = true;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (parse_options && arg == "--") {
      parse_options = false;
      continue;
    }

    if (parse_options && (arg == "-h" || arg == "--help")) {
      cfg.show_help = true;
      continue;
    }

    if (parse_options && arg == "--metadata-dir") {
      if (i + 1 >= argc) {
        return std::unexpected{"missing path after --metadata-dir"};
      }
      cfg.metadata_dir = argv[++i];
      if (cfg.metadata_dir.empty()) {
        return std::unexpected{"--metadata-dir requires a non-empty path"};
      }
      continue;
    }

    if (parse_options && arg.starts_with("--metadata-dir=")) {
      cfg.metadata_dir = std::string(arg.substr(std::string_view{"--metadata-dir="}.size()));
      if (cfg.metadata_dir.empty()) {
        return std::unexpected{"--metadata-dir requires a non-empty path"};
      }
      continue;
    }

    if (parse_options && arg.starts_with('-') && arg != "-") {
      return std::unexpected{std::format("unknown option: {}", arg)};
    }

    cfg.sources.emplace_back(arg);
  }

  return cfg;
}

/// Render the user-facing CLI help text.
///
/// @param program_name Executable name to display in the usage line.
auto render_help(std::string_view program_name) -> std::string {
  return std::format(
      "Usage: {} [OPTIONS] SOURCES...\n\n"
      "Kira - Parse source files and emit module metadata\n\n"
      "Options:\n"
      "  -h, --help          Show this help message and exit\n"
      "  --metadata-dir PATH Write module metadata under PATH\n"
      "                     (default: {})",
      program_name, kDefaultMetadataDir);
}

/// Parse, validate, and emit metadata for each requested source file.
///
/// @param cfg Command-line configuration that names source inputs and outputs.
/// @param use_color Whether rendered diagnostics should include ANSI colors.
auto compile_sources(const cli_config &cfg, bool use_color)
    -> std::expected<compile_report, std::string> {
  if (cfg.sources.empty()) {
    return std::unexpected{"no source files provided"};
  }

  auto report = compile_report{};
  const auto metadata_root = fs::path(cfg.metadata_dir);
  auto sources = source_manager{};
  auto session_diagnostics = diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto parsed_inputs = std::vector<parsed_input>{};

  for (const auto &source_arg : cfg.sources) {
    const auto source_path = fs::path(source_arg);
    auto source_text = read_source_file(source_path);
    if (!source_text) {
      ++report.error_count;
      append_error(report.diagnostics, source_text.error());
      continue;
    }

    auto file_id =
        sources.add_file(normalize_path(source_path), std::move(*source_text));
    if (!file_id) {
      ++report.error_count;
      append_error(report.diagnostics, file_id.error());
      continue;
    }

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    if (file == nullptr) {
      ++report.error_count;
      append_error(report.diagnostics, "internal error: missing source file");
      continue;
    }

    auto errors_before = session_diagnostics.error_count();
    auto lexer = Lexer(file->source(), file->id(), session_diagnostics);
    auto tokens = lexer.tokenize();
    auto parser = kira::parser(std::move(tokens), file->id(), session_diagnostics);
    auto ast_file = parser.parse_file();

    if (session_diagnostics.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_inputs.push_back(parsed_input{
        .source_path = source_path,
        .file_id = *file_id,
        .ast_file = std::move(ast_file),
    });
  }

  const auto session_index = build_module_session_index(parsed_inputs);
  detect_duplicate_module_paths(parsed_inputs, session_diagnostics,
                                file_has_errors);
  validate_module_boundaries(session_index, session_diagnostics, file_has_errors);
  validate_session_imports(parsed_inputs, session_index, session_diagnostics,
                           file_has_errors);

  report.error_count += session_diagnostics.error_count();
  append_text(report.diagnostics,
              diagnostic_renderer(sources, use_color).render_all(session_diagnostics));

  for (const auto &input : parsed_inputs) {
    if (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }

    if (input.ast_file == nullptr) {
      continue;
    }

    auto metadata_path = write_module_metadata(metadata_root, *input.ast_file,
                                               input.source_path,
                                               report.diagnostics);
    if (!metadata_path) {
      ++report.error_count;
      continue;
    }

    report.modules.push_back(compiled_module{
        .source_path = normalize_path(input.source_path),
        .module_path = input.ast_file->module_decl != nullptr
                           ? input.ast_file->module_decl->path
                           : std::vector<std::string>{},
        .metadata_path = std::move(*metadata_path),
    });
  }

  return report;
}

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
auto render_compile_summary(const compile_report &report) -> std::string {
  if (report.modules.empty()) {
    return std::format("Compilation failed with {} error(s).", report.error_count);
  }

  auto out = std::format("Compiled {} module(s):", report.modules.size());
  for (size_t i = 0; i < report.modules.size(); ++i) {
    out += std::format("\n  [{}] {} -> {}", i, module_display_name(report.modules[i]),
                       report.modules[i].metadata_path);
  }
  if (report.error_count > 0) {
    out += std::format("\nEncountered {} error(s).", report.error_count);
  }
  return out;
}

} // namespace kira
