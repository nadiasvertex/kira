#include "cli.h"

#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "k-parser/parser.h"
#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
#include "src/hir/lower.h"
#include "src/llvm_codegen/aot.h"
#include "src/llvm_codegen/codegen.h"
#include "src/module_metadata.pb.h"
#include "src/semantic/analysis.h"
#include "src/semantic/types.h"

namespace kira {
namespace {

namespace fs = std::filesystem;

/// File extension used for serialized module metadata artifacts.
constexpr std::string_view kMetadataExtension = ".kmeta.pb";

/// Schema version embedded in every emitted module metadata payload.
constexpr uint32_t kModuleMetadataSchemaVersion = 1;

/// Append text to a diagnostic buffer, inserting a separating newline when
/// needed.
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

/// Convert a filesystem path to the normalized slash-separated form used in
/// reports.
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

/// Choose a stable fallback stem for metadata when a source file has no
/// filename stem.
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
[[nodiscard]] auto module_display_name(const compiled_module &module)
    -> std::string {
  if (!module.module_path.empty()) {
    return join_strings(module.module_path, ".");
  }
  return module.source_path;
}

/// Return the parent module portion of a dotted module path.
///
/// @param module_name Canonical dotted module name.
[[nodiscard]] auto parent_module_name(std::string_view module_name)
    -> std::string {
  const auto separator = module_name.rfind('.');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string(module_name.substr(0, separator));
}

/// Return the root segment of a dotted module path.
///
/// @param module_name Canonical dotted module name.
[[nodiscard]] auto module_root_name(std::string_view module_name)
    -> std::string_view {
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

/// Report whether `module_name` is the same module as `ancestor` or one of its
/// descendants.
///
/// @param module_name Candidate descendant module path.
/// @param ancestor Candidate ancestor module path.
[[nodiscard]] auto is_same_or_descendant_module(std::string_view module_name,
                                                std::string_view ancestor)
    -> bool {
  if (ancestor.empty()) {
    return false;
  }
  return module_name == ancestor || (module_name.starts_with(ancestor) &&
                                     module_name.size() > ancestor.size() &&
                                     module_name[ancestor.size()] == '.');
}

/// Render user-facing visibility text for diagnostics.
///
/// @param visibility Visibility modifier attached to a declaration.
[[nodiscard]] auto visibility_name(ast::visibility visibility)
    -> std::string_view {
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
                                   std::string_view parent_name)
    -> std::string {
  switch (visibility) {
  case ast::visibility::pub:
    return std::format(
        "Mark the module `pub` only when it should be importable outside `{}`.",
        parent_name);
  case ast::visibility::def:
  case ast::visibility::internal:
    return std::format("Import this module from `{}` or one of its submodules, "
                       "or widen the declaration's visibility.",
                       parent_name);
  case ast::visibility::super:
    return std::format("Only the parent module `{}` can import this module; "
                       "widen the declaration if other modules need it.",
                       parent_name);
  case ast::visibility::priv:
    return "Keep the import in the declaring file, or widen the module "
           "declaration's visibility.";
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
[[nodiscard]] auto selector_kind_to_proto(ast::use_selector_kind kind)
    -> metadata::v1::ImportSelectorKind {
  switch (kind) {
  case ast::use_selector_kind::Single:
    return metadata::v1::IMPORT_SELECTOR_KIND_SINGLE;
  case ast::use_selector_kind::Group:
    return metadata::v1::IMPORT_SELECTOR_KIND_GROUP;
  case ast::use_selector_kind::Wildcard:
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

/// Choose a stable display label for metadata emitted from `static`
/// declarations.
///
/// @param decl Static declaration to describe.
[[nodiscard]] auto static_decl_label(const ast::static_decl &decl)
    -> std::string {
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
[[nodiscard]] auto top_level_visibility(const ast::node &node)
    -> ast::visibility {
  switch (node.kind) {
  case ast::node_kind::use_decl:
    return dynamic_cast<const ast::use_decl &>(node).visibility;
  case ast::node_kind::type_decl:
    return dynamic_cast<const ast::type_decl &>(node).visibility;
  case ast::node_kind::trait_decl:
    return dynamic_cast<const ast::trait_decl &>(node).visibility;
  case ast::node_kind::concept_decl:
    return dynamic_cast<const ast::concept_decl &>(node).visibility;
  case ast::node_kind::func_decl:
    return dynamic_cast<const ast::func_decl &>(node).visibility;
  case ast::node_kind::sub_module_decl:
    return dynamic_cast<const ast::sub_module_decl &>(node).visibility;
  case ast::node_kind::static_decl:
    return dynamic_cast<const ast::static_decl &>(node).visibility;
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
    return join_strings(dynamic_cast<const ast::use_decl &>(node).path, ".");
  case ast::node_kind::type_decl:
    return dynamic_cast<const ast::type_decl &>(node).name;
  case ast::node_kind::trait_decl:
    return dynamic_cast<const ast::trait_decl &>(node).name;
  case ast::node_kind::concept_decl:
    return dynamic_cast<const ast::concept_decl &>(node).name;
  case ast::node_kind::func_decl:
    return dynamic_cast<const ast::func_decl &>(node).name;
  case ast::node_kind::sub_module_decl:
    return dynamic_cast<const ast::sub_module_decl &>(node).name;
  case ast::node_kind::dep_decl:
    return dynamic_cast<const ast::dep_decl &>(node).name;
  case ast::node_kind::static_decl:
    return static_decl_label(dynamic_cast<const ast::static_decl &>(node));
  case ast::node_kind::error_node:
    return dynamic_cast<const ast::error_node &>(node).description;
  default:
    return {};
  }
}

/// Remove surrounding quotes from dependency string literals before persisting
/// them.
///
/// @param value Parsed dependency field value.
[[nodiscard]] auto unquote_string_literal(std::string_view value)
    -> std::string {
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
    (*dependency->mutable_fields())[field.key] =
        unquote_string_literal(field.value);
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
      add_import_metadata(dynamic_cast<const ast::use_decl &>(*item), metadata);
      break;
    case ast::node_kind::dep_decl:
      add_dependency_metadata(dynamic_cast<const ast::dep_decl &>(*item),
                              metadata);
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
                                        const fs::path &source_path)
    -> fs::path {
  auto relative = fs::path{};

  if (file.module_decl != nullptr && !file.module_decl->path.empty()) {
    for (size_t i = 0; i + 1 < file.module_decl->path.size(); ++i) {
      relative /= file.module_decl->path[i];
    }
    relative /= file.module_decl->path.back() + std::string(kMetadataExtension);
    return metadata_root / relative;
  }

  relative /=
      source_stem_or_default(source_path) + std::string(kMetadataExtension);
  return metadata_root / relative;
}

/// Serialize one module's metadata file and return its output path.
///
/// @param metadata_root Root directory configured for metadata output.
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
/// @param diagnostics Plain-text driver diagnostics buffer for I/O failures.
[[nodiscard]] auto
write_module_metadata(const fs::path &metadata_root, const ast::file &file,
                      const fs::path &source_path, std::string &diagnostics)
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
    append_error(diagnostics, std::format("failed to open `{}` for writing",
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
  fs::path source_path;     ///< Original source file path passed to the CLI.
  file_id_type file_id = 0; ///< Source manager file identifier for diagnostics.
  ast::ptr<ast::file> ast_file; ///< Parsed AST for the source file.
};

// The original phase-3 semantic implementation below has been extracted into
// `src/semantic/`. Keep the old in-file copy disabled until the extraction has
// fully settled, so the driver can delegate cleanly without duplicate symbols.
#if 0
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

/// One name introduced into the module-local declaration scope.
///
/// Phase 3 begins by indexing declaration names that feed type and module
/// resolution before broader value-level name resolution lands.
enum class module_scope_symbol_kind {
  type_symbol,
  trait_symbol,
  concept_symbol,
  submodule_symbol,
  function_symbol,
  static_binding_symbol,
};

struct module_scope_symbol_record {
  std::string name;         ///< Symbol name introduced into the enclosing module scope.
  module_scope_symbol_kind kind; ///< Semantic kind used by later resolution passes.
  std::string kind_name;    ///< User-facing declaration kind for diagnostics.
  ast::visibility visibility = ast::visibility::def; ///< Visibility of the declaration.
  source_location location; ///< Declaration site for duplicate-name notes.
};

/// One module body plus the declarations it contributes to semantic path resolution.
struct module_scope_record {
  std::string module_name; ///< Fully-qualified module scope name.
  file_id_type file_id = 0; ///< Source manager file identifier for the scope owner.
  std::vector<module_scope_symbol_record>
      symbols; ///< Declarations introduced directly in this module body.
};

/// Session-local symbol tables used by the phase-3 path resolver.
struct semantic_resolution_index {
  std::vector<module_scope_record>
      module_scopes; ///< File and inline-module scopes available for resolution.
};

/// Report whether a declaration kind participates in duplicate-name checks.
///
/// @param kind Indexed declaration kind.
[[nodiscard]] constexpr auto
participates_in_duplicate_module_scope_check(module_scope_symbol_kind kind) -> bool {
  switch (kind) {
  case module_scope_symbol_kind::type_symbol:
  case module_scope_symbol_kind::trait_symbol:
  case module_scope_symbol_kind::concept_symbol:
  case module_scope_symbol_kind::submodule_symbol:
    return true;
  case module_scope_symbol_kind::function_symbol:
  case module_scope_symbol_kind::static_binding_symbol:
    return false;
  }
  return false;
}

/// Report whether a declaration kind is usable in type-position path resolution.
///
/// @param kind Indexed declaration kind.
[[nodiscard]] constexpr auto can_resolve_named_type(module_scope_symbol_kind kind)
    -> bool {
  switch (kind) {
  case module_scope_symbol_kind::type_symbol:
  case module_scope_symbol_kind::trait_symbol:
  case module_scope_symbol_kind::concept_symbol:
  case module_scope_symbol_kind::submodule_symbol:
    return true;
  case module_scope_symbol_kind::function_symbol:
  case module_scope_symbol_kind::static_binding_symbol:
    return false;
  }
  return false;
}

/// Report whether a declaration kind is usable in module-qualified value references.
///
/// @param kind Indexed declaration kind.
[[nodiscard]] constexpr auto
can_resolve_module_reference(module_scope_symbol_kind kind) -> bool {
  switch (kind) {
  case module_scope_symbol_kind::type_symbol:
  case module_scope_symbol_kind::trait_symbol:
  case module_scope_symbol_kind::concept_symbol:
  case module_scope_symbol_kind::submodule_symbol:
  case module_scope_symbol_kind::function_symbol:
  case module_scope_symbol_kind::static_binding_symbol:
    return true;
  }
  return false;
}

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

/// Extract one declaration that participates in the module-local type/module scope.
///
/// @param node Parsed item declared directly in a module body.
/// @param file_id Source manager file identifier for the enclosing file.
[[nodiscard]] auto module_scope_symbol(const ast::node &node,
                                       file_id_type file_id)
    -> std::optional<module_scope_symbol_record> {
  switch (node.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = static_cast<const ast::type_decl &>(node);
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::type_symbol,
        .kind_name = "type",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::trait_decl: {
    const auto &decl = static_cast<const ast::trait_decl &>(node);
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::trait_symbol,
        .kind_name = "trait",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::concept_decl: {
    const auto &decl = static_cast<const ast::concept_decl &>(node);
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::concept_symbol,
        .kind_name = "concept",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::sub_module_decl: {
    const auto &decl = static_cast<const ast::sub_module_decl &>(node);
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::submodule_symbol,
        .kind_name = "submodule",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::func_decl: {
    const auto &decl = static_cast<const ast::func_decl &>(node);
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::function_symbol,
        .kind_name = "function",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  case ast::node_kind::static_decl: {
    const auto &decl = static_cast<const ast::static_decl &>(node);
    if (decl.decl_kind != ast::static_decl_kind::binding || decl.name.empty()) {
      return std::nullopt;
    }
    return module_scope_symbol_record{
        .name = decl.name,
        .kind = module_scope_symbol_kind::static_binding_symbol,
        .kind_name = "static binding",
        .visibility = decl.visibility,
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
    };
  }
  default:
    return std::nullopt;
  }
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

/// Split one dotted module name into its path segments.
///
/// @param module_name Canonical dotted module name.
[[nodiscard]] auto split_module_name(std::string_view module_name)
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

/// Collect the declarations directly introduced by one module body.
///
/// @param items Parsed items declared directly in the current module body.
/// @param module_name Fully-qualified module scope name.
/// @param file_id Source manager file identifier for the enclosing file.
/// @param out Destination vector for collected module scopes.
auto collect_module_scopes(const std::vector<ast::ptr<ast::node>> &items,
                           std::string_view module_name, file_id_type file_id,
                           std::vector<module_scope_record> &out) -> void {
  auto scope = module_scope_record{
      .module_name = std::string(module_name),
      .file_id = file_id,
      .symbols = {},
  };

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }
    if (const auto symbol = module_scope_symbol(*item, file_id)) {
      scope.symbols.push_back(*symbol);
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

/// Build the phase-3 symbol tables used by qualified-path validation.
///
/// @param inputs Parsed source files in the current compilation session.
[[nodiscard]] auto
build_semantic_resolution_index(const std::vector<parsed_input> &inputs)
    -> semantic_resolution_index {
  auto index = semantic_resolution_index{.module_scopes = {}};

  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty()) {
      continue;
    }

    collect_module_scopes(input.ast_file->items,
                          join_strings(input.ast_file->module_decl->path, "."),
                          input.file_id, index.module_scopes);
  }

  return index;
}

/// Look up one module-local symbol table by its fully-qualified scope name.
///
/// @param index Phase-3 symbol tables.
/// @param module_name Fully-qualified module scope name.
[[nodiscard]] auto find_module_scope(const semantic_resolution_index &index,
                                     std::string_view module_name)
    -> const module_scope_record * {
  for (const auto &scope : index.module_scopes) {
    if (scope.module_name == module_name) {
      return &scope;
    }
  }
  return nullptr;
}

/// Look up one direct declaration inside a known module scope.
///
/// @param index Phase-3 symbol tables.
/// @param module_name Fully-qualified module scope name.
/// @param symbol_name Declaration name to resolve inside that scope.
[[nodiscard]] auto find_module_scope_symbol(const semantic_resolution_index &index,
                                            std::string_view module_name,
                                            std::string_view symbol_name)
    -> const module_scope_symbol_record * {
  const auto *scope = find_module_scope(index, module_name);
  if (scope == nullptr) {
    return nullptr;
  }

  for (const auto &symbol : scope->symbols) {
    if (symbol.name == symbol_name) {
      return &symbol;
    }
  }
  return nullptr;
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

/// Emit one duplicate-declaration diagnostic for a module-local name conflict.
///
/// @param module_name Fully-qualified module scope that contains the conflict.
/// @param current Current declaration that duplicates an earlier name.
/// @param previous First declaration that claimed the same name in this scope.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_duplicate_module_scope_symbol(
    std::string_view module_name, const module_scope_symbol_record &current,
    const module_scope_symbol_record &previous, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void {
  auto duplicate = diagnostic(
      diagnostic_level::error,
      std::format("duplicate declaration name `{}` in module `{}`", current.name,
                  module_name),
      current.location.file_id);
  duplicate.with_label(current.location.span,
                       std::format("duplicate {} declaration", current.kind_name));
  duplicate.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}` was first declared here as a {}", current.name,
                             previous.kind_name),
                 previous.location.file_id)
          .with_label(previous.location.span,
                      std::format("previous {} declaration", previous.kind_name)));
  duplicate.with_help(
      "Give each type, trait, concept, and submodule in a module scope a unique "
      "name before semantic analysis continues.");
  diag.emit(std::move(duplicate));
  mark_file_has_error(file_has_errors, current.location.file_id);
}

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

    if (decl.selector->kind == ast::use_selector_kind::Wildcard) {
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

/// Validate one module body's declaration scope, then recurse into inline children.
///
/// @param items Parsed items declared directly in the current module body.
/// @param module_name Fully-qualified name of the current module scope.
/// @param file_id Source manager file identifier for the enclosing file.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto validate_module_scope(const std::vector<ast::ptr<ast::node>> &items,
                           std::string_view module_name, file_id_type file_id,
                           diagnostic_bag &diag,
                           std::vector<bool> &file_has_errors) -> void {
  auto seen_symbols = std::vector<module_scope_symbol_record>{};

  for (const auto &item : items) {
    if (item == nullptr || item->has_error) {
      continue;
    }

    if (const auto symbol = module_scope_symbol(*item, file_id);
        symbol && participates_in_duplicate_module_scope_check(symbol->kind)) {
      const module_scope_symbol_record *previous = nullptr;
      for (const auto &candidate : seen_symbols) {
        if (candidate.name == symbol->name) {
          previous = &candidate;
          break;
        }
      }

      if (previous != nullptr) {
        emit_duplicate_module_scope_symbol(module_name, *symbol, *previous, diag,
                                           file_has_errors);
      } else {
        seen_symbols.push_back(*symbol);
      }
    }

    if (item->kind != ast::node_kind::sub_module_decl) {
      continue;
    }

    const auto &decl = static_cast<const ast::sub_module_decl &>(*item);
    if (!decl.items.empty()) {
      validate_module_scope(decl.items, append_module_name(module_name, decl.name),
                            file_id, diag, file_has_errors);
    }
  }
}

/// Build the first phase-3 declaration scopes and reject duplicate names.
///
/// @param inputs Parsed source files in the current compilation session.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto validate_declaration_scopes(const std::vector<parsed_input> &inputs,
                                 diagnostic_bag &diag,
                                 std::vector<bool> &file_has_errors) -> void {
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }

    validate_module_scope(input.ast_file->items,
                          join_strings(input.ast_file->module_decl->path, "."),
                          input.file_id, diag, file_has_errors);
  }
}

enum class qualified_path_status {
  resolved,
  unresolved,
  blocked,
};

/// One attempted qualified-path resolution during the phase-3 semantic walk.
struct qualified_path_resolution {
  qualified_path_status status = qualified_path_status::unresolved;
  std::string absolute_path; ///< Absolute module-qualified path that was checked.
  std::string containing_module_name; ///< Deepest module prefix found in-session.
  size_t resolved_module_prefix = 0;  ///< Count of path segments resolved as a module.
  const module_scope_symbol_record *symbol = nullptr; ///< Final symbol, when found.
};

/// Current semantic walk location for one file/module body.
struct semantic_walk_context {
  std::string module_name; ///< Fully-qualified module that owns the walked AST node.
  file_id_type file_id = 0; ///< Source manager file identifier for diagnostics.
};

/// Report whether this named type is specific enough for the phase-3 resolver.
///
/// Bare single-segment names are deferred to fuller symbol-table work so this
/// pass can focus on module-qualified paths without guessing about preludes or
/// local type bindings.
///
/// @param type Parsed named type syntax.
[[nodiscard]] auto should_validate_named_type_path(const ast::named_type &type) -> bool {
  return !type.path.empty() &&
         (type.path.front() == "super" || type.path.size() > 1);
}

/// Report whether this dotted expression is explicit enough to treat as a module path.
///
/// Dotted expressions like `self.x` and `point.y` are still ambiguous without
/// value-level name resolution, so this pass only validates paths that clearly
/// start at `super` or a session-owned root module namespace.
///
/// @param expr Parsed dotted expression.
/// @param index Session-wide module graph facts.
[[nodiscard]] auto should_validate_module_reference(
    const ast::module_path_expr &expr, const module_session_index &index) -> bool {
  return !expr.segments.empty() &&
         (expr.segments.front() == "super" ||
          session_owns_root_module(index, expr.segments.front()));
}

/// Score unresolved resolution attempts so diagnostics can report the best context.
///
/// @param candidate Candidate resolution result to compare.
/// @param current Current best result.
[[nodiscard]] auto is_better_resolution_candidate(
    const qualified_path_resolution &candidate,
    const qualified_path_resolution &current) -> bool {
  if (candidate.status != current.status) {
    if (candidate.status == qualified_path_status::resolved) {
      return true;
    }
    if (current.status == qualified_path_status::resolved) {
      return false;
    }
    if (candidate.status == qualified_path_status::blocked) {
      return true;
    }
    if (current.status == qualified_path_status::blocked) {
      return false;
    }
  }

  if (candidate.resolved_module_prefix != current.resolved_module_prefix) {
    return candidate.resolved_module_prefix > current.resolved_module_prefix;
  }
  return candidate.symbol != nullptr && current.symbol == nullptr;
}

/// Resolve one already-absolute qualified path against session modules and module scopes.
///
/// @param semantic_index Phase-3 module-local symbol tables.
/// @param session_index Session-wide module graph facts.
/// @param absolute_segments Absolute module-qualified path segments to resolve.
/// @param file_has_errors Per-file error flags used to suppress cascades.
[[nodiscard]] auto resolve_absolute_qualified_path(
    const semantic_resolution_index &semantic_index,
    const module_session_index &session_index,
    const std::vector<std::string> &absolute_segments,
    const std::vector<bool> &file_has_errors) -> qualified_path_resolution {
  auto result = qualified_path_resolution{
      .status = qualified_path_status::unresolved,
      .absolute_path = join_strings(absolute_segments, "."),
      .containing_module_name = {},
      .resolved_module_prefix = 0,
      .symbol = nullptr,
  };

  if (absolute_segments.empty()) {
    result.status = qualified_path_status::resolved;
    return result;
  }

  for (size_t i = 1; i <= absolute_segments.size(); ++i) {
    const auto prefix = join_module_path_prefix(absolute_segments, i);
    if (!session_contains_module(session_index, prefix)) {
      continue;
    }

    result.resolved_module_prefix = i;
    result.containing_module_name = prefix;
  }

  if (result.resolved_module_prefix == 0) {
    return result;
  }

  if (module_resolution_blocked_by_errors(session_index,
                                          result.containing_module_name,
                                          file_has_errors)) {
    result.status = qualified_path_status::blocked;
    return result;
  }

  if (result.resolved_module_prefix == absolute_segments.size()) {
    result.status = qualified_path_status::resolved;
    return result;
  }

  result.symbol = find_module_scope_symbol(
      semantic_index, result.containing_module_name,
      absolute_segments[result.resolved_module_prefix]);
  if (result.symbol == nullptr) {
    return result;
  }

  if (result.resolved_module_prefix + 1 == absolute_segments.size()) {
    result.status = qualified_path_status::resolved;
  }

  return result;
}

/// Resolve one type-position path from the current module scope.
///
/// @param semantic_index Phase-3 module-local symbol tables.
/// @param session_index Session-wide module graph facts.
/// @param current_module_name Fully-qualified module containing the reference.
/// @param path Parsed type-position path to resolve.
/// @param file_has_errors Per-file error flags used to suppress cascades.
[[nodiscard]] auto resolve_named_type_path(
    const semantic_resolution_index &semantic_index,
    const module_session_index &session_index, std::string_view current_module_name,
    const std::vector<std::string> &path,
    const std::vector<bool> &file_has_errors) -> qualified_path_resolution {
  if (path.empty()) {
    return qualified_path_resolution{.status = qualified_path_status::resolved};
  }

  if (path.front() == "super") {
    const auto parent_name = parent_module_name(current_module_name);
    if (parent_name.empty()) {
      return qualified_path_resolution{};
    }
    if (path.size() == 1) {
      return qualified_path_resolution{
          .status = qualified_path_status::resolved,
          .absolute_path = parent_name,
          .containing_module_name = parent_name,
          .resolved_module_prefix = split_module_name(parent_name).size(),
          .symbol = nullptr,
      };
    }

    auto absolute_segments = split_module_name(parent_name);
    absolute_segments.insert(absolute_segments.end(), path.begin() + 1, path.end());
    return resolve_absolute_qualified_path(semantic_index, session_index,
                                           absolute_segments, file_has_errors);
  }

  auto best = qualified_path_resolution{};

  auto relative_segments = split_module_name(current_module_name);
  relative_segments.insert(relative_segments.end(), path.begin(), path.end());
  auto relative = resolve_absolute_qualified_path(semantic_index, session_index,
                                                  relative_segments,
                                                  file_has_errors);
  best = std::move(relative);

  if (session_owns_root_module(session_index, path.front())) {
    auto absolute = resolve_absolute_qualified_path(semantic_index, session_index,
                                                    path, file_has_errors);
    if (is_better_resolution_candidate(absolute, best)) {
      best = std::move(absolute);
    }
  }

  return best;
}

/// Resolve one explicit module-qualified expression path.
///
/// @param semantic_index Phase-3 module-local symbol tables.
/// @param session_index Session-wide module graph facts.
/// @param current_module_name Fully-qualified module containing the reference.
/// @param path Parsed dotted expression path to resolve.
/// @param file_has_errors Per-file error flags used to suppress cascades.
[[nodiscard]] auto resolve_module_reference_path(
    const semantic_resolution_index &semantic_index,
    const module_session_index &session_index, std::string_view current_module_name,
    const std::vector<std::string> &path,
    const std::vector<bool> &file_has_errors) -> qualified_path_resolution {
  if (path.empty()) {
    return qualified_path_resolution{.status = qualified_path_status::resolved};
  }

  if (path.front() == "super") {
    const auto parent_name = parent_module_name(current_module_name);
    if (parent_name.empty()) {
      return qualified_path_resolution{};
    }

    auto absolute_segments = split_module_name(parent_name);
    absolute_segments.insert(absolute_segments.end(), path.begin() + 1, path.end());
    return resolve_absolute_qualified_path(semantic_index, session_index,
                                           absolute_segments, file_has_errors);
  }

  return resolve_absolute_qualified_path(semantic_index, session_index, path,
                                         file_has_errors);
}

/// Emit one diagnostic for `super` used where no parent module exists.
///
/// @param kind_name User-facing description of the path context.
/// @param path_text Original source path text.
/// @param location Source location of the path.
/// @param module_name Fully-qualified module containing the path.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_invalid_super_path(std::string_view kind_name, std::string_view path_text,
                             source_location location,
                             std::string_view module_name, diagnostic_bag &diag,
                             std::vector<bool> &file_has_errors) -> void {
  auto invalid = diagnostic(
      diagnostic_level::error,
      std::format("{} `{}` cannot be resolved from root module `{}`", kind_name,
                  path_text, module_name),
      location.file_id);
  invalid.with_label(location.span, "`super` has no parent module here");
  invalid.with_help(
      "Use a session-owned module path, or remove the parent-qualified prefix.");
  diag.emit(std::move(invalid));
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Emit one unresolved-path diagnostic for the phase-3 qualified-path resolver.
///
/// @param kind_name User-facing description of the path context.
/// @param path_text Original source path text.
/// @param location Source location of the path.
/// @param module_name Fully-qualified module containing the path.
/// @param resolution Best-effort resolution details gathered by the walker.
/// @param session_index Session-wide module graph facts for related notes.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_unresolved_qualified_path(
    std::string_view kind_name, std::string_view path_text,
    source_location location, std::string_view module_name,
    const qualified_path_resolution &resolution,
    const module_session_index &session_index, diagnostic_bag &diag,
    std::vector<bool> &file_has_errors) -> void {
  auto unresolved = diagnostic(
      diagnostic_level::error,
      std::format("{} `{}` does not resolve from module `{}`", kind_name,
                  path_text, module_name),
      location.file_id);
  unresolved.with_label(location.span, std::format("unresolved {}", kind_name));

  if (!resolution.absolute_path.empty() && resolution.absolute_path != path_text) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("checked absolute path `{}`",
                               resolution.absolute_path),
                   location.file_id));
  }

  if (resolution.symbol != nullptr) {
    unresolved.children.push_back(
        diagnostic(diagnostic_level::note,
                   std::format("`{}` is declared here as a {}",
                               resolution.symbol->name,
                               resolution.symbol->kind_name),
                   resolution.symbol->location.file_id)
            .with_label(resolution.symbol->location.span,
                        "path stops at this declaration"));
  } else if (!resolution.containing_module_name.empty()) {
    if (const auto anchor =
            find_nearest_module_anchor(session_index,
                                       resolution.containing_module_name)) {
      unresolved.children.push_back(
          diagnostic(diagnostic_level::note,
                     std::format("module `{}` is declared here",
                                 anchor->module_name),
                     anchor->location.file_id)
              .with_label(anchor->location.span, "nearest resolved module"));
    }
  }

  unresolved.with_help(
      "Declare the referenced symbol in that module, or change the path to one "
      "that exists in this compilation session.");
  diag.emit(std::move(unresolved));
  mark_file_has_error(file_has_errors, location.file_id);
}

/// Emit one wrong-kind diagnostic when a type-position path ends at a non-type symbol.
///
/// @param path_text Original source path text.
/// @param location Source location of the path.
/// @param resolution Successful resolution details ending at a declaration.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this diagnostic.
auto emit_non_type_qualified_path(std::string_view path_text,
                                  source_location location,
                                  const qualified_path_resolution &resolution,
                                  diagnostic_bag &diag,
                                  std::vector<bool> &file_has_errors) -> void {
  if (resolution.symbol == nullptr) {
    return;
  }

  auto wrong_kind = diagnostic(
      diagnostic_level::error,
      std::format("qualified type path `{}` resolves to a {}, not a type",
                  path_text, resolution.symbol->kind_name),
      location.file_id);
  wrong_kind.with_label(location.span, "used here as a type");
  wrong_kind.children.push_back(
      diagnostic(diagnostic_level::note,
                 std::format("`{}` is declared here as a {}",
                             resolution.symbol->name,
                             resolution.symbol->kind_name),
                 resolution.symbol->location.file_id)
          .with_label(resolution.symbol->location.span,
                      std::format("{} declaration", resolution.symbol->kind_name)));
  wrong_kind.with_help(
      "Type positions currently accept types, traits, concepts, and modules only.");
  diag.emit(std::move(wrong_kind));
  mark_file_has_error(file_has_errors, location.file_id);
}

auto validate_ast_node(const ast::node &node, const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag,
                       std::vector<bool> &file_has_errors) -> void;

auto validate_type_expr(const ast::type_expr &type,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void;

auto validate_expr(const ast::expr &expr, const semantic_walk_context &context,
                   const semantic_resolution_index &semantic_index,
                   const module_session_index &session_index,
                   diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors) -> void;

auto validate_pattern(const ast::pattern &pattern,
                      const semantic_walk_context &context,
                      const semantic_resolution_index &semantic_index,
                      const module_session_index &session_index,
                      diagnostic_bag &diag,
                      std::vector<bool> &file_has_errors) -> void;

auto validate_node_list(const std::vector<ast::ptr<ast::node>> &items,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void;

auto validate_bound(const ast::bound &bound, const semantic_walk_context &context,
                    const semantic_resolution_index &semantic_index,
                    const module_session_index &session_index,
                    diagnostic_bag &diag,
                    std::vector<bool> &file_has_errors) -> void {
  for (const auto &term : bound.terms) {
    if (term.type != nullptr) {
      validate_type_expr(*term.type, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
  }
}

auto validate_type_param(const ast::type_param &param,
                         const semantic_walk_context &context,
                         const semantic_resolution_index &semantic_index,
                         const module_session_index &session_index,
                         diagnostic_bag &diag,
                         std::vector<bool> &file_has_errors) -> void {
  if (param.bound_or_type != nullptr) {
    validate_type_expr(*param.bound_or_type, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

auto validate_where_constraint(const ast::where_constraint &constraint,
                               const semantic_walk_context &context,
                               const semantic_resolution_index &semantic_index,
                               const module_session_index &session_index,
                               diagnostic_bag &diag,
                               std::vector<bool> &file_has_errors) -> void {
  if (constraint.subject != nullptr) {
    validate_type_expr(*constraint.subject, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
  if (constraint.bound_or_type != nullptr) {
    validate_type_expr(*constraint.bound_or_type, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

auto validate_param(const ast::param &param, const semantic_walk_context &context,
                    const semantic_resolution_index &semantic_index,
                    const module_session_index &session_index,
                    diagnostic_bag &diag,
                    std::vector<bool> &file_has_errors) -> void {
  if (param.pattern != nullptr) {
    validate_pattern(*param.pattern, context, semantic_index, session_index, diag,
                     file_has_errors);
  }
  if (param.type_annotation != nullptr) {
    validate_type_expr(*param.type_annotation, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
  if (param.default_value != nullptr) {
    validate_expr(*param.default_value, context, semantic_index, session_index,
                  diag, file_has_errors);
  }
}

auto validate_type_expr(const ast::type_expr &type,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  if (type.has_error) {
    return;
  }

  switch (type.kind) {
  case ast::node_kind::named_type: {
    const auto &named = static_cast<const ast::named_type &>(type);
    for (const auto &arg : named.type_args) {
      if (arg.value != nullptr) {
        validate_ast_node(*arg.value, context, semantic_index, session_index, diag,
                          file_has_errors);
      }
    }

    if (!should_validate_named_type_path(named) || named.path.empty()) {
      return;
    }

    const auto path_text = join_strings(named.path, ".");
    const auto location = source_location{
        .file_id = context.file_id,
        .span = named.span,
    };

    if (named.path.front() == "super" &&
        parent_module_name(context.module_name).empty()) {
      emit_invalid_super_path("qualified type path", path_text, location,
                              context.module_name, diag, file_has_errors);
      return;
    }

    const auto resolution = resolve_named_type_path(
        semantic_index, session_index, context.module_name, named.path,
        file_has_errors);
    if (resolution.status == qualified_path_status::blocked) {
      return;
    }
    if (resolution.status != qualified_path_status::resolved) {
      emit_unresolved_qualified_path("qualified type path", path_text, location,
                                     context.module_name, resolution,
                                     session_index, diag, file_has_errors);
      return;
    }
    if (resolution.symbol != nullptr &&
        !can_resolve_named_type(resolution.symbol->kind)) {
      emit_non_type_qualified_path(path_text, location, resolution, diag,
                                   file_has_errors);
    }
    return;
  }

  case ast::node_kind::bound_type:
    validate_bound(static_cast<const ast::bound_type &>(type).value, context,
                   semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::tuple_type: {
    const auto &tuple = static_cast<const ast::tuple_type &>(type);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_type_expr(*element, context, semantic_index, session_index, diag,
                           file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::slice_type: {
    const auto &slice = static_cast<const ast::slice_type &>(type);
    if (slice.element != nullptr) {
      validate_type_expr(*slice.element, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::array_type: {
    const auto &array = static_cast<const ast::array_type &>(type);
    if (array.element != nullptr) {
      validate_type_expr(*array.element, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (array.size != nullptr) {
      validate_ast_node(*array.size, context, semantic_index, session_index, diag,
                        file_has_errors);
    }
    return;
  }

  case ast::node_kind::ref_type: {
    const auto &ref = static_cast<const ast::ref_type &>(type);
    if (ref.inner != nullptr) {
      validate_type_expr(*ref.inner, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    return;
  }

  case ast::node_kind::ptr_type: {
    const auto &ptr = static_cast<const ast::ptr_type &>(type);
    if (ptr.inner != nullptr) {
      validate_type_expr(*ptr.inner, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    return;
  }

  case ast::node_kind::fn_type: {
    const auto &fn = static_cast<const ast::fn_type &>(type);
    for (const auto &param_type : fn.param_types) {
      if (param_type != nullptr) {
        validate_type_expr(*param_type, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    if (fn.return_type != nullptr) {
      validate_type_expr(*fn.return_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::union_type: {
    const auto &union_type = static_cast<const ast::union_type &>(type);
    for (const auto &alternative : union_type.alternatives) {
      if (alternative != nullptr) {
        validate_type_expr(*alternative, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::refinement_type: {
    const auto &refinement = static_cast<const ast::refinement_type &>(type);
    if (refinement.base != nullptr) {
      validate_type_expr(*refinement.base, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (refinement.predicate != nullptr) {
      validate_ast_node(*refinement.predicate, context, semantic_index,
                        session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::quote_type:
    return;

  default:
    return;
  }
}

auto validate_pattern(const ast::pattern &pattern,
                      const semantic_walk_context &context,
                      const semantic_resolution_index &semantic_index,
                      const module_session_index &session_index,
                      diagnostic_bag &diag,
                      std::vector<bool> &file_has_errors) -> void {
  if (pattern.has_error) {
    return;
  }

  switch (pattern.kind) {
  case ast::node_kind::constructor_pattern: {
    const auto &ctor = static_cast<const ast::constructor_pattern &>(pattern);
    for (const auto &arg : ctor.args) {
      if (arg != nullptr) {
        validate_pattern(*arg, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::tuple_pattern: {
    const auto &tuple = static_cast<const ast::tuple_pattern &>(pattern);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_pattern(*element, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::struct_pattern: {
    const auto &struct_pattern = static_cast<const ast::struct_pattern &>(pattern);
    for (const auto &field : struct_pattern.fields) {
      if (field.pattern != nullptr) {
        validate_pattern(*field.pattern, context, semantic_index, session_index,
                         diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::array_pattern: {
    const auto &array = static_cast<const ast::array_pattern &>(pattern);
    for (const auto &element : array.elements) {
      if (element != nullptr) {
        validate_pattern(*element, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::range_pattern: {
    const auto &range = static_cast<const ast::range_pattern &>(pattern);
    if (range.start != nullptr) {
      validate_expr(*range.start, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (range.end != nullptr) {
      validate_expr(*range.end, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::option_pattern: {
    const auto &option = static_cast<const ast::option_pattern &>(pattern);
    if (option.inner != nullptr) {
      validate_pattern(*option.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::result_pattern: {
    const auto &result = static_cast<const ast::result_pattern &>(pattern);
    if (result.inner != nullptr) {
      validate_pattern(*result.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::ref_pattern: {
    const auto &ref = static_cast<const ast::ref_pattern &>(pattern);
    if (ref.inner != nullptr) {
      validate_pattern(*ref.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  case ast::node_kind::or_pattern: {
    const auto &or_pattern = static_cast<const ast::or_pattern &>(pattern);
    for (const auto &alternative : or_pattern.alternatives) {
      if (alternative != nullptr) {
        validate_pattern(*alternative, context, semantic_index, session_index,
                         diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::group_pattern: {
    const auto &group = static_cast<const ast::group_pattern &>(pattern);
    if (group.inner != nullptr) {
      validate_pattern(*group.inner, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    return;
  }

  default:
    return;
  }
}

auto validate_expr(const ast::expr &expr, const semantic_walk_context &context,
                   const semantic_resolution_index &semantic_index,
                   const module_session_index &session_index,
                   diagnostic_bag &diag,
                   std::vector<bool> &file_has_errors) -> void {
  if (expr.has_error) {
    return;
  }

  switch (expr.kind) {
  case ast::node_kind::binary_expr: {
    const auto &binary = static_cast<const ast::binary_expr &>(expr);
    if (binary.lhs != nullptr) {
      validate_expr(*binary.lhs, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (binary.rhs != nullptr) {
      validate_expr(*binary.rhs, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::unary_expr: {
    const auto &unary = static_cast<const ast::unary_expr &>(expr);
    if (unary.operand != nullptr) {
      validate_expr(*unary.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::call_expr: {
    const auto &call = static_cast<const ast::call_expr &>(expr);
    if (call.callee != nullptr) {
      validate_expr(*call.callee, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arg : call.args) {
      if (arg.value != nullptr) {
        validate_expr(*arg.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::index_expr: {
    const auto &index = static_cast<const ast::index_expr &>(expr);
    if (index.object != nullptr) {
      validate_expr(*index.object, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (index.index != nullptr) {
      validate_expr(*index.index, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::field_expr: {
    const auto &field = static_cast<const ast::field_expr &>(expr);
    if (field.object != nullptr) {
      validate_expr(*field.object, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &generic_arg : field.generic_args) {
      if (generic_arg != nullptr) {
        validate_type_expr(*generic_arg, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::cast_expr: {
    const auto &cast = static_cast<const ast::cast_expr &>(expr);
    if (cast.operand != nullptr) {
      validate_expr(*cast.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (cast.target_type != nullptr) {
      validate_type_expr(*cast.target_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::try_expr: {
    const auto &try_expr = static_cast<const ast::try_expr &>(expr);
    if (try_expr.operand != nullptr) {
      validate_expr(*try_expr.operand, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::tuple_expr: {
    const auto &tuple = static_cast<const ast::tuple_expr &>(expr);
    for (const auto &element : tuple.elements) {
      if (element != nullptr) {
        validate_expr(*element, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::array_expr: {
    const auto &array = static_cast<const ast::array_expr &>(expr);
    for (const auto &element : array.elements) {
      if (element != nullptr) {
        validate_expr(*element, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    if (array.fill_value != nullptr) {
      validate_expr(*array.fill_value, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (array.fill_count != nullptr) {
      validate_expr(*array.fill_count, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::struct_expr: {
    const auto &struct_expr = static_cast<const ast::struct_expr &>(expr);
    if (struct_expr.type_name != nullptr) {
      validate_expr(*struct_expr.type_name, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    for (const auto &field : struct_expr.fields) {
      if (field.value != nullptr) {
        validate_expr(*field.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::lambda_expr: {
    const auto &lambda = static_cast<const ast::lambda_expr &>(expr);
    for (const auto &param : lambda.params) {
      if (param.pattern != nullptr) {
        validate_ast_node(*param.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (param.type_annotation != nullptr) {
        validate_type_expr(*param.type_annotation, context, semantic_index,
                           session_index, diag, file_has_errors);
      }
    }
    if (lambda.return_type != nullptr) {
      validate_type_expr(*lambda.return_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (lambda.body_expr != nullptr) {
      validate_expr(*lambda.body_expr, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    validate_node_list(lambda.body_stmts, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::module_path_expr: {
    const auto &path = static_cast<const ast::module_path_expr &>(expr);
    if (!should_validate_module_reference(path, session_index) ||
        path.segments.empty()) {
      return;
    }

    const auto path_text = join_strings(path.segments, ".");
    const auto location = source_location{
        .file_id = context.file_id,
        .span = path.span,
    };

    if (path.segments.front() == "super" &&
        parent_module_name(context.module_name).empty()) {
      emit_invalid_super_path("module-qualified reference", path_text, location,
                              context.module_name, diag, file_has_errors);
      return;
    }

    const auto resolution = resolve_module_reference_path(
        semantic_index, session_index, context.module_name, path.segments,
        file_has_errors);
    if (resolution.status == qualified_path_status::blocked) {
      return;
    }
    if (resolution.status != qualified_path_status::resolved) {
      emit_unresolved_qualified_path(
          "module-qualified reference", path_text, location, context.module_name,
          resolution, session_index, diag, file_has_errors);
      return;
    }
    if (resolution.symbol != nullptr &&
        !can_resolve_module_reference(resolution.symbol->kind)) {
      emit_unresolved_qualified_path(
          "module-qualified reference", path_text, location, context.module_name,
          resolution, session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::group_expr: {
    const auto &group = static_cast<const ast::group_expr &>(expr);
    if (group.inner != nullptr) {
      validate_expr(*group.inner, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::if_expr: {
    const auto &if_expr = static_cast<const ast::if_expr &>(expr);
    for (const auto &branch : if_expr.branches) {
      if (branch.condition != nullptr) {
        validate_expr(*branch.condition, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      if (branch.let_pattern != nullptr) {
        validate_ast_node(*branch.let_pattern, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
      if (branch.let_expr != nullptr) {
        validate_expr(*branch.let_expr, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      validate_node_list(branch.body, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    validate_node_list(if_expr.else_body, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::match_expr: {
    const auto &match = static_cast<const ast::match_expr &>(expr);
    if (match.subject != nullptr) {
      validate_expr(*match.subject, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arm : match.arms) {
      if (arm.pattern != nullptr) {
        validate_ast_node(*arm.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (arm.guard != nullptr) {
        validate_expr(*arm.guard, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      if (arm.body_expr != nullptr) {
        validate_expr(*arm.body_expr, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      validate_node_list(arm.body_stmts, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::for_expr: {
    const auto &for_expr = static_cast<const ast::for_expr &>(expr);
    for (const auto &clause : for_expr.clauses) {
      for (const auto &pattern : clause.patterns) {
        if (pattern != nullptr) {
          validate_ast_node(*pattern, context, semantic_index, session_index, diag,
                            file_has_errors);
        }
      }
      if (clause.iterable != nullptr) {
        validate_expr(*clause.iterable, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
    }
    if (for_expr.guard != nullptr) {
      validate_expr(*for_expr.guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (for_expr.yield_expr != nullptr) {
      validate_expr(*for_expr.yield_expr, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::await_expr: {
    const auto &await = static_cast<const ast::await_expr &>(expr);
    if (await.operand != nullptr) {
      validate_expr(*await.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::async_expr:
    validate_node_list(static_cast<const ast::async_expr &>(expr).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::par_expr: {
    const auto &par = static_cast<const ast::par_expr &>(expr);
    for (const auto &branch : par.branches) {
      if (branch != nullptr) {
        validate_expr(*branch, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::race_expr: {
    const auto &race = static_cast<const ast::race_expr &>(expr);
    for (const auto &branch : race.branches) {
      if (branch != nullptr) {
        validate_expr(*branch, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::crew_expr:
    validate_node_list(static_cast<const ast::crew_expr &>(expr).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::on_expr: {
    const auto &on = static_cast<const ast::on_expr &>(expr);
    if (on.context_type != nullptr) {
      validate_type_expr(*on.context_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (on.sender != nullptr) {
      validate_expr(*on.sender, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(on.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::block_expr:
    validate_node_list(static_cast<const ast::block_expr &>(expr).stmts, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::splice_expr: {
    const auto &splice = static_cast<const ast::splice_expr &>(expr);
    if (splice.operand != nullptr) {
      validate_expr(*splice.operand, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::static_expr: {
    const auto &static_expr = static_cast<const ast::static_expr &>(expr);
    if (static_expr.operand != nullptr) {
      validate_expr(*static_expr.operand, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::where_expr: {
    const auto &where = static_cast<const ast::where_expr &>(expr);
    if (where.inner != nullptr) {
      validate_expr(*where.inner, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &binding : where.bindings) {
      if (binding.value != nullptr) {
        validate_expr(*binding.value, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
    }
    return;
  }

  default:
    return;
  }
}

auto validate_ast_node(const ast::node &node, const semantic_walk_context &context,
                       const semantic_resolution_index &semantic_index,
                       const module_session_index &session_index,
                       diagnostic_bag &diag,
                       std::vector<bool> &file_has_errors) -> void {
  if (node.has_error) {
    return;
  }

  switch (node.kind) {
  case ast::node_kind::type_decl: {
    const auto &decl = static_cast<const ast::type_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.definition != nullptr) {
      validate_ast_node(*decl.definition, context, semantic_index, session_index,
                        diag, file_has_errors);
    }
    if (decl.invariant != nullptr) {
      validate_expr(*decl.invariant, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::struct_type_def: {
    const auto &def = static_cast<const ast::struct_type_def &>(node);
    for (const auto &field : def.body.fields) {
      if (field.type != nullptr) {
        validate_type_expr(*field.type, context, semantic_index, session_index,
                           diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::sum_type_def: {
    const auto &def = static_cast<const ast::sum_type_def &>(node);
    for (const auto &variant : def.body.variants) {
      for (const auto &payload : variant.payload_types) {
        if (payload != nullptr) {
          validate_type_expr(*payload, context, semantic_index, session_index,
                             diag, file_has_errors);
        }
      }
    }
    return;
  }

  case ast::node_kind::trait_decl: {
    const auto &decl = static_cast<const ast::trait_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.requires_bound.has_value()) {
      validate_bound(*decl.requires_bound, context, semantic_index, session_index,
                     diag, file_has_errors);
    }
    validate_node_list(decl.items, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::associated_type_decl_node: {
    const auto &assoc = static_cast<const ast::associated_type_decl_node &>(node);
    if (assoc.value.default_type != nullptr) {
      validate_type_expr(*assoc.value.default_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::associated_type_def_node: {
    const auto &assoc = static_cast<const ast::associated_type_def_node &>(node);
    if (assoc.value.type != nullptr) {
      validate_type_expr(*assoc.value.type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::concept_decl: {
    const auto &decl = static_cast<const ast::concept_decl &>(node);
    for (const auto &constraint : decl.constraints) {
      if (constraint.subject != nullptr) {
        validate_type_expr(*constraint.subject, context, semantic_index,
                           session_index, diag, file_has_errors);
      }
      if (constraint.bound_or_expr != nullptr) {
        validate_ast_node(*constraint.bound_or_expr, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
    }
    return;
  }

  case ast::node_kind::impl_decl: {
    const auto &decl = static_cast<const ast::impl_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.trait_type != nullptr) {
      validate_type_expr(*decl.trait_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    if (decl.for_type != nullptr) {
      validate_type_expr(*decl.for_type, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    for (const auto &constraint : decl.where_constraints) {
      validate_where_constraint(constraint, context, semantic_index,
                                session_index, diag, file_has_errors);
    }
    validate_node_list(decl.items, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::func_decl: {
    const auto &decl = static_cast<const ast::func_decl &>(node);
    for (const auto &param : decl.type_params) {
      validate_type_param(param, context, semantic_index, session_index, diag,
                          file_has_errors);
    }
    if (decl.modifiers.async_context != nullptr) {
      validate_type_expr(*decl.modifiers.async_context, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    for (const auto &param : decl.params) {
      validate_param(param, context, semantic_index, session_index, diag,
                     file_has_errors);
    }
    if (decl.return_type != nullptr) {
      validate_type_expr(*decl.return_type, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    for (const auto &constraint : decl.where_constraints) {
      validate_where_constraint(constraint, context, semantic_index,
                                session_index, diag, file_has_errors);
    }
    for (const auto &contract : decl.contracts) {
      if (contract.condition != nullptr) {
        validate_expr(*contract.condition, context, semantic_index,
                      session_index, diag, file_has_errors);
      }
    }
    if (decl.body_expr != nullptr) {
      validate_expr(*decl.body_expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(decl.body_stmts, context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::sub_module_decl: {
    const auto &decl = static_cast<const ast::sub_module_decl &>(node);
    auto child_context = semantic_walk_context{
        .module_name = append_module_name(context.module_name, decl.name),
        .file_id = context.file_id,
    };
    validate_node_list(decl.items, child_context, semantic_index, session_index,
                       diag, file_has_errors);
    return;
  }

  case ast::node_kind::static_decl: {
    const auto &decl = static_cast<const ast::static_decl &>(node);
    if (decl.type_annotation != nullptr) {
      validate_type_expr(*decl.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (decl.initializer != nullptr) {
      validate_expr(*decl.initializer, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (decl.assert_condition != nullptr) {
      validate_expr(*decl.assert_condition, context, semantic_index,
                    session_index, diag, file_has_errors);
    }
    if (decl.if_condition != nullptr) {
      validate_expr(*decl.if_condition, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    validate_node_list(decl.if_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    validate_node_list(decl.else_body, context, semantic_index, session_index,
                       diag, file_has_errors);
    for (const auto &pattern : decl.for_patterns) {
      if (pattern != nullptr) {
        validate_pattern(*pattern, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    if (decl.for_iterable != nullptr) {
      validate_expr(*decl.for_iterable, context, semantic_index, session_index,
                    diag, file_has_errors);
    }
    if (decl.for_guard != nullptr) {
      validate_expr(*decl.for_guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (decl.for_yield != nullptr) {
      validate_expr(*decl.for_yield, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(decl.for_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::let_stmt: {
    const auto &stmt = static_cast<const ast::let_stmt &>(node);
    if (stmt.pattern != nullptr) {
      validate_pattern(*stmt.pattern, context, semantic_index, session_index, diag,
                       file_has_errors);
    }
    if (stmt.type_annotation != nullptr) {
      validate_type_expr(*stmt.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (stmt.initializer != nullptr) {
      validate_expr(*stmt.initializer, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.else_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::var_stmt: {
    const auto &stmt = static_cast<const ast::var_stmt &>(node);
    if (stmt.type_annotation != nullptr) {
      validate_type_expr(*stmt.type_annotation, context, semantic_index,
                         session_index, diag, file_has_errors);
    }
    if (stmt.initializer != nullptr) {
      validate_expr(*stmt.initializer, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::assign_stmt: {
    const auto &stmt = static_cast<const ast::assign_stmt &>(node);
    if (stmt.target != nullptr) {
      validate_expr(*stmt.target, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.value != nullptr) {
      validate_expr(*stmt.value, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::expr_stmt: {
    const auto &stmt = static_cast<const ast::expr_stmt &>(node);
    if (stmt.expr != nullptr) {
      validate_expr(*stmt.expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::return_stmt: {
    const auto &stmt = static_cast<const ast::return_stmt &>(node);
    if (stmt.value != nullptr) {
      validate_expr(*stmt.value, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::if_stmt: {
    const auto &stmt = static_cast<const ast::if_stmt &>(node);
    for (const auto &branch : stmt.branches) {
      if (branch.condition != nullptr) {
        validate_expr(*branch.condition, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      if (branch.let_pattern != nullptr) {
        validate_ast_node(*branch.let_pattern, context, semantic_index,
                          session_index, diag, file_has_errors);
      }
      if (branch.let_expr != nullptr) {
        validate_expr(*branch.let_expr, context, semantic_index, session_index,
                      diag, file_has_errors);
      }
      validate_node_list(branch.body, context, semantic_index, session_index, diag,
                         file_has_errors);
    }
    validate_node_list(stmt.else_body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::while_stmt: {
    const auto &stmt = static_cast<const ast::while_stmt &>(node);
    if (stmt.condition != nullptr) {
      validate_expr(*stmt.condition, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.let_pattern != nullptr) {
      validate_pattern(*stmt.let_pattern, context, semantic_index, session_index,
                       diag, file_has_errors);
    }
    if (stmt.let_expr != nullptr) {
      validate_expr(*stmt.let_expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::for_stmt: {
    const auto &stmt = static_cast<const ast::for_stmt &>(node);
    for (const auto &pattern : stmt.patterns) {
      if (pattern != nullptr) {
        validate_pattern(*pattern, context, semantic_index, session_index, diag,
                         file_has_errors);
      }
    }
    if (stmt.iterable != nullptr) {
      validate_expr(*stmt.iterable, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    if (stmt.guard != nullptr) {
      validate_expr(*stmt.guard, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    validate_node_list(stmt.body, context, semantic_index, session_index, diag,
                       file_has_errors);
    return;
  }

  case ast::node_kind::match_stmt: {
    const auto &stmt = static_cast<const ast::match_stmt &>(node);
    if (stmt.subject != nullptr) {
      validate_expr(*stmt.subject, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    for (const auto &arm : stmt.arms) {
      if (arm.pattern != nullptr) {
        validate_ast_node(*arm.pattern, context, semantic_index, session_index,
                          diag, file_has_errors);
      }
      if (arm.guard != nullptr) {
        validate_expr(*arm.guard, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      if (arm.body_expr != nullptr) {
        validate_expr(*arm.body_expr, context, semantic_index, session_index, diag,
                      file_has_errors);
      }
      validate_node_list(arm.body_stmts, context, semantic_index, session_index,
                         diag, file_has_errors);
    }
    return;
  }

  case ast::node_kind::crew_stmt:
    validate_node_list(static_cast<const ast::crew_stmt &>(node).body, context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::splice_stmt: {
    const auto &stmt = static_cast<const ast::splice_stmt &>(node);
    if (stmt.expr != nullptr) {
      validate_expr(*stmt.expr, context, semantic_index, session_index, diag,
                    file_has_errors);
    }
    return;
  }

  case ast::node_kind::named_type:
  case ast::node_kind::bound_type:
  case ast::node_kind::tuple_type:
  case ast::node_kind::slice_type:
  case ast::node_kind::array_type:
  case ast::node_kind::ref_type:
  case ast::node_kind::ptr_type:
  case ast::node_kind::fn_type:
  case ast::node_kind::quote_type:
  case ast::node_kind::union_type:
  case ast::node_kind::refinement_type:
    validate_type_expr(static_cast<const ast::type_expr &>(node), context,
                       semantic_index, session_index, diag, file_has_errors);
    return;

  case ast::node_kind::ident_expr:
  case ast::node_kind::literal_expr:
  case ast::node_kind::binary_expr:
  case ast::node_kind::unary_expr:
  case ast::node_kind::call_expr:
  case ast::node_kind::index_expr:
  case ast::node_kind::field_expr:
  case ast::node_kind::cast_expr:
  case ast::node_kind::try_expr:
  case ast::node_kind::tuple_expr:
  case ast::node_kind::array_expr:
  case ast::node_kind::struct_expr:
  case ast::node_kind::lambda_expr:
  case ast::node_kind::match_expr:
  case ast::node_kind::if_expr:
  case ast::node_kind::for_expr:
  case ast::node_kind::await_expr:
  case ast::node_kind::async_expr:
  case ast::node_kind::par_expr:
  case ast::node_kind::race_expr:
  case ast::node_kind::crew_expr:
  case ast::node_kind::on_expr:
  case ast::node_kind::block_expr:
  case ast::node_kind::quote_expr:
  case ast::node_kind::splice_expr:
  case ast::node_kind::static_expr:
  case ast::node_kind::module_path_expr:
  case ast::node_kind::group_expr:
  case ast::node_kind::where_expr:
    validate_expr(static_cast<const ast::expr &>(node), context, semantic_index,
                  session_index, diag, file_has_errors);
    return;

  case ast::node_kind::wildcard_pattern:
  case ast::node_kind::literal_pattern:
  case ast::node_kind::binding_pattern:
  case ast::node_kind::constructor_pattern:
  case ast::node_kind::tuple_pattern:
  case ast::node_kind::struct_pattern:
  case ast::node_kind::array_pattern:
  case ast::node_kind::range_pattern:
  case ast::node_kind::option_pattern:
  case ast::node_kind::result_pattern:
  case ast::node_kind::ref_pattern:
  case ast::node_kind::or_pattern:
  case ast::node_kind::group_pattern:
    validate_pattern(static_cast<const ast::pattern &>(node), context,
                     semantic_index, session_index, diag, file_has_errors);
    return;

  default:
    return;
  }
}

auto validate_node_list(const std::vector<ast::ptr<ast::node>> &items,
                        const semantic_walk_context &context,
                        const semantic_resolution_index &semantic_index,
                        const module_session_index &session_index,
                        diagnostic_bag &diag,
                        std::vector<bool> &file_has_errors) -> void {
  for (const auto &item : items) {
    if (item == nullptr) {
      continue;
    }
    validate_ast_node(*item, context, semantic_index, session_index, diag,
                      file_has_errors);
  }
}

/// Resolve named-type paths and explicit module-qualified references.
///
/// @param inputs Parsed source files in the current compilation session.
/// @param session_index Session-wide module graph facts.
/// @param semantic_index Phase-3 module-local symbol tables.
/// @param diag Session diagnostic bag.
/// @param file_has_errors Per-file error flags updated by this pass.
auto validate_qualified_paths(const std::vector<parsed_input> &inputs,
                              const module_session_index &session_index,
                              const semantic_resolution_index &semantic_index,
                              diagnostic_bag &diag,
                              std::vector<bool> &file_has_errors) -> void {
  for (const auto &input : inputs) {
    if (input.ast_file == nullptr || input.ast_file->module_decl == nullptr ||
        input.ast_file->module_decl->has_error ||
        input.ast_file->module_decl->path.empty() ||
        has_file_error(file_has_errors, input.file_id)) {
      continue;
    }

    auto context = semantic_walk_context{
        .module_name = join_strings(input.ast_file->module_decl->path, "."),
        .file_id = input.file_id,
    };
    validate_node_list(input.ast_file->items, context, semantic_index,
                       session_index, diag, file_has_errors);
  }
}

#endif

/// Renders a VM return value for `--run`'s output, using the function's
/// checked return type to pick how `slot_value`'s untagged union should be
/// read back (mirrors `bytecode_compiler::encode_literal`'s reverse
/// direction).
///
/// @param types Checked type table the function's `return_type` indexes
/// into.
/// @param return_type Checked return type of the executed function.
/// @param value Raw VM result to render.
[[nodiscard]] auto render_run_value(const semantic::type_table &types,
                                    semantic::type_id return_type,
                                    const bytecode::slot_value &value)
    -> std::string {
  const auto kind = bytecode::numeric_kind_of(types, return_type);
  if (!kind) {
    return "<unsupported return type>";
  }

  switch (*kind) {
  case bytecode::numeric_kind::i8:
  case bytecode::numeric_kind::i16:
  case bytecode::numeric_kind::i32:
  case bytecode::numeric_kind::i64:
    return std::format("{}", value.i);
  case bytecode::numeric_kind::u8:
  case bytecode::numeric_kind::u16:
  case bytecode::numeric_kind::u32:
  case bytecode::numeric_kind::u64:
    return std::format("{}", value.u);
  case bytecode::numeric_kind::f32:
    return std::format("{}",
                       std::bit_cast<float>(static_cast<uint32_t>(value.u)));
  case bytecode::numeric_kind::f64:
    return std::format("{}", value.f);
  case bytecode::numeric_kind::boolean:
    return value.i != 0 ? "true" : "false";
  case bytecode::numeric_kind::character:
    return std::format("U+{:04X}", value.u);
  }
  return {};
}

/// Compiles `module` to bytecode and executes `function_name` with no
/// arguments, producing the `--run` outcome shown in the CLI summary.
/// `--run` targets exactly `spec/codegen-design.md` increment 1's subset —
/// this project has no CLI-level argument-marshalling story yet, so only
/// zero-parameter functions are runnable — and fails closed (a message, not
/// a crash) on every other reason execution can't proceed.
///
/// @param hir_module Lowered module to compile and run.
/// @param types Checked type table the module's HIR indexes into.
/// @param function_name Name of the zero-argument function to execute.
[[nodiscard]] auto run_hir_module(const hir::hir_module &hir_module,
                                  const semantic::type_table &types,
                                  std::string_view function_name)
    -> run_outcome {
  const auto *target = static_cast<const hir::hir_function *>(nullptr);
  for (const auto &fn : hir_module.functions) {
    if (fn != nullptr && fn->name == function_name) {
      target = fn.get();
      break;
    }
  }

  if (target == nullptr) {
    return run_outcome{.succeeded = false,
                       .message =
                           std::format("module `{}` has no function named `{}`",
                                       hir_module.module_name, function_name)};
  }

  if (!target->params.empty()) {
    return run_outcome{
        .succeeded = false,
        .message = std::format(
            "cannot run `{}`: --run only supports zero-parameter functions",
            function_name)};
  }

  auto compiled = bytecode_compiler::compile_module(hir_module, types);
  if (!compiled) {
    return run_outcome{.succeeded = false,
                       .message = std::format("failed to compile `{}` to "
                                              "bytecode: {}",
                                              function_name,
                                              compiled.error().message)};
  }

  auto index = uint16_t{0};
  for (; index < compiled->functions.size(); ++index) {
    if (compiled->functions[index].name == function_name) {
      break;
    }
  }

  const auto vm = bytecode::vm{*compiled};
  auto result = vm.run(index, std::span<const bytecode::slot_value>{});
  if (!result) {
    return run_outcome{
        .succeeded = false,
        .message = std::format("`{}` panicked: {}", function_name,
                               bytecode::panic_reason_message(result.error()))};
  }

  if (!result->has_value) {
    return run_outcome{.succeeded = true,
                       .message = std::format("{}() -> ()", function_name)};
  }

  return run_outcome{
      .succeeded = true,
      .message = std::format(
          "{}() -> {}", function_name,
          render_run_value(types, target->return_type, result->value))};
}

/// Locates one `alwayslink = True` cc_library's archive under `bazel_package`
/// (e.g. `src/llvm_codegen`) named `library_name` (e.g. `aot_runtime`), so
/// `--build` can hand it to the system linker. There is no installed-location
/// story yet (`just package` doesn't bundle these) — this only finds an
/// archive when `kira` itself is run from within the Bazel workspace that
/// built it (via `bazelisk run //src:kira` or directly from `bazel-bin`),
/// mirroring how `driver_stress_test.cpp`/`semantic_stress_test.cpp` locate
/// their own test corpora through Bazel runfiles.
///
/// @param program_name `argv[0]` this process was invoked with.
[[nodiscard]] auto find_bazel_archive(std::string_view program_name,
                                      std::string_view bazel_package,
                                      std::string_view library_name)
    -> std::optional<fs::path> {
  auto candidates = std::vector<fs::path>{};
  // Bazel names an `alwayslink = True` cc_library's archive `.lo`, not
  // `.a` (still an ordinary `ar` archive under the hood) — check both
  // since that's an implementation detail of the Bazel version in use, not
  // something this driver should hardcode a single answer for.
  for (const auto *extension : {"lo", "a"}) {
    const auto filename = std::format("lib{}.{}", library_name, extension);
    if (!program_name.empty()) {
      candidates.emplace_back(
          fs::path(std::format("{}.runfiles", program_name)) / "_main" /
          bazel_package / filename);
    }
    candidates.emplace_back(fs::path("bazel-bin") / bazel_package / filename);
  }

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

/// Compiles `hir_module` to a native object file via `src/llvm_codegen`,
/// then links it against Kira's AOT runtime support library into a
/// standalone executable at `output_path` — `--build`'s counterpart to
/// `run_hir_module`, using the same "zero-argument entry function" scope
/// limit (`llvm_codegen::emit_object_file`'s own doc comment) and the same
/// fail-closed-with-a-message discipline.
///
/// @param hir_module Lowered module to compile and link.
/// @param types Checked type table the module's HIR indexes into.
/// @param function_name Name of the zero-argument function to use as the
/// executable's entry point.
/// @param output_path Destination path for the linked executable.
/// @param program_name `argv[0]`, used to locate the AOT runtime archive.
[[nodiscard]] auto build_hir_module(const hir::hir_module &hir_module,
                                    const semantic::type_table &types,
                                    std::string_view function_name,
                                    const fs::path &output_path,
                                    std::string_view program_name)
    -> build_outcome {
  auto compiled = llvm_codegen::compile_module(hir_module, types);
  if (!compiled) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("failed to compile `{}` to native code: {}",
                               function_name, compiled.error().message)};
  }

  const auto object_path =
      fs::temp_directory_path() /
      std::format("kira-build-{}.o", static_cast<long>(::getpid()));
  auto emitted = llvm_codegen::emit_object_file(
      std::move(*compiled), function_name, object_path.string());
  if (!emitted) {
    return build_outcome{.succeeded = false,
                         .message = std::format("failed to emit an object "
                                                "file for `{}`: {}",
                                                function_name,
                                                emitted.error().message)};
  }

  const auto panic_archive =
      find_bazel_archive(program_name, "src/llvm_codegen", "aot_runtime");
  if (!panic_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's AOT panic runtime support "
                   "library (libaot_runtime.a) — run `kira` via `bazelisk "
                   "run //src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  // Statically links the same bump-arena heap allocator (`kira_rt_alloc`,
  // src/runtime/arena.h) `llvm_codegen`-compiled IR calls for every
  // non-scalar value (spec/codegen-design.md Decision 3) — an AOT binary
  // has no libLLVM/bytecode-VM dependency (Decision 5), but it does still
  // need this one small runtime support library, the same as the panic
  // archive above.
  const auto heap_archive =
      find_bazel_archive(program_name, "src/runtime", "runtime");
  if (!heap_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's heap runtime support library "
                   "(libruntime.a) — run `kira` via `bazelisk run "
                   "//src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }

  const auto link_command = std::format(
      R"(cc "{}" "{}" "{}" -o "{}")", object_path.string(),
      panic_archive->string(), heap_archive->string(), output_path.string());
  const auto link_status = std::system(link_command.c_str());
  auto ec = std::error_code{};
  fs::remove(object_path, ec);
  if (link_status != 0) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("linking failed (exit status {})", link_status)};
  }

  return build_outcome{.succeeded = true, .message = output_path.string()};
}

} // namespace

/// Parse CLI arguments into the compile driver's configuration structure.
///
/// @param argc Argument count passed to `main`.
/// @param argv Argument vector passed to `main`.
auto parse_args(std::span<char *const> argv)
    -> std::expected<cli_config, std::string> {
  if (argv.empty()) {
    return std::unexpected{"argc must be at least 1"};
  }

  auto cfg = cli_config{
      .program_name = argv[0],
      .sources = {},
      .metadata_dir = std::string(kDefaultMetadataDir),
      .show_help = false,
  };

  bool parse_options = true;
  for (size_t i = 1; i < argv.size(); ++i) {
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
      if (i + 1 >= argv.size()) {
        return std::unexpected{"missing path after --metadata-dir"};
      }
      cfg.metadata_dir = argv[++i];
      if (cfg.metadata_dir.empty()) {
        return std::unexpected{"--metadata-dir requires a non-empty path"};
      }
      continue;
    }

    if (parse_options && arg.starts_with("--metadata-dir=")) {
      cfg.metadata_dir =
          std::string(arg.substr(std::string_view{"--metadata-dir="}.size()));
      if (cfg.metadata_dir.empty()) {
        return std::unexpected{"--metadata-dir requires a non-empty path"};
      }
      continue;
    }

    if (parse_options && arg == "--run") {
      cfg.run = true;
      continue;
    }

    if (parse_options && arg == "--run-function") {
      if (i + 1 >= argv.size()) {
        return std::unexpected{"missing name after --run-function"};
      }
      cfg.run = true;
      cfg.run_function = argv[++i];
      if (cfg.run_function.empty()) {
        return std::unexpected{"--run-function requires a non-empty name"};
      }
      continue;
    }

    if (parse_options && arg.starts_with("--run-function=")) {
      cfg.run = true;
      cfg.run_function =
          std::string(arg.substr(std::string_view{"--run-function="}.size()));
      if (cfg.run_function.empty()) {
        return std::unexpected{"--run-function requires a non-empty name"};
      }
      continue;
    }

    if (parse_options && arg == "--build") {
      cfg.build = true;
      continue;
    }

    if (parse_options && arg == "--build-function") {
      if (i + 1 >= argv.size()) {
        return std::unexpected{"missing name after --build-function"};
      }
      cfg.build = true;
      cfg.build_function = argv[++i];
      if (cfg.build_function.empty()) {
        return std::unexpected{"--build-function requires a non-empty name"};
      }
      continue;
    }

    if (parse_options && arg.starts_with("--build-function=")) {
      cfg.build = true;
      cfg.build_function =
          std::string(arg.substr(std::string_view{"--build-function="}.size()));
      if (cfg.build_function.empty()) {
        return std::unexpected{"--build-function requires a non-empty name"};
      }
      continue;
    }

    if (parse_options && arg == "--build-output") {
      if (i + 1 >= argv.size()) {
        return std::unexpected{"missing path after --build-output"};
      }
      cfg.build = true;
      cfg.build_output = argv[++i];
      if (cfg.build_output.empty()) {
        return std::unexpected{"--build-output requires a non-empty path"};
      }
      continue;
    }

    if (parse_options && arg.starts_with("--build-output=")) {
      cfg.build = true;
      cfg.build_output =
          std::string(arg.substr(std::string_view{"--build-output="}.size()));
      if (cfg.build_output.empty()) {
        return std::unexpected{"--build-output requires a non-empty path"};
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
      "  -h, --help             Show this help message and exit\n"
      "  --metadata-dir PATH    Write module metadata under PATH\n"
      "                        (default: {})\n"
      "  --run                  Compile to bytecode and execute `{}` via the\n"
      "                        tier-0 VM (increment 1's scalar/control-flow\n"
      "                        subset only; see src/bytecode_compiler)\n"
      "  --run-function NAME    Like --run, but execute NAME instead of `{}`\n"
      "  --build                Compile `{}` to native code via LLVM, link a\n"
      "                        standalone executable (same scalar/control-\n"
      "                        flow subset; see src/llvm_codegen)\n"
      "  --build-function NAME  Like --build, but use NAME as the entry point\n"
      "  --build-output PATH    Write the linked executable to PATH",
      program_name, kDefaultMetadataDir, kDefaultRunFunction,
      kDefaultRunFunction, kDefaultRunFunction);
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
    auto tokenizer = lexer(file->source(), file->id(), session_diagnostics);
    auto tokens = tokenizer.tokenize();
    auto parser =
        kira::parser(std::move(tokens), file->id(), session_diagnostics);
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

  auto semantic_inputs = std::vector<semantic::parsed_module>{};
  semantic_inputs.reserve(parsed_inputs.size());
  for (const auto &input : parsed_inputs) {
    semantic_inputs.push_back(semantic::parsed_module{
        .file_id = input.file_id,
        .ast_file = input.ast_file.get(),
    });
  }
  // Consumed below to best-effort lower each successfully-checked module to
  // HIR (src/hir/lower.h); meaningless (empty) when cfg.parse_only skipped
  // checking, so lowering is skipped in that case too.
  const auto checked = semantic::validate_semantics(
      semantic_inputs, session_diagnostics, file_has_errors,
      semantic::semantic_options{.check_names_and_types = !cfg.parse_only});

  report.error_count += session_diagnostics.error_count();
  append_text(
      report.diagnostics,
      diagnostic_renderer(sources, use_color).render_all(session_diagnostics));

  auto lowered_modules = hir::ptr_vec<hir::hir_module>{};

  for (const auto &input : parsed_inputs) {
    if (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }

    if (input.ast_file == nullptr) {
      continue;
    }

    auto metadata_path = write_module_metadata(
        metadata_root, *input.ast_file, input.source_path, report.diagnostics);
    if (!metadata_path) {
      ++report.error_count;
      continue;
    }

    const auto module_path = input.ast_file->module_decl != nullptr
                                 ? input.ast_file->module_decl->path
                                 : std::vector<std::string>{};
    const auto module_name = join_strings(module_path, ".");

    report.modules.push_back(compiled_module{
        .source_path = normalize_path(input.source_path),
        .module_path = module_path,
        .metadata_path = std::move(*metadata_path),
    });

    // Best-effort only (see hir_lowering_result's doc comment): lowering
    // coverage is still partial, so a failure here is not a compile error.
    if (!cfg.parse_only) {
      auto lowered = hir::lower_module(*input.ast_file, module_name, checked);
      report.hir_modules.push_back(
          lowered.has_value()
              ? hir_lowering_result{.module_path = module_name, .lowered = true}
              : hir_lowering_result{.module_path = module_name,
                                    .lowered = false,
                                    .error = lowered.error().message});
      if (lowered.has_value()) {
        lowered_modules.push_back(std::move(*lowered));
      }
    }
  }

  if (cfg.run) {
    const auto *target_module = static_cast<const hir::hir_module *>(nullptr);
    for (const auto &module : lowered_modules) {
      if (module == nullptr) {
        continue;
      }
      const auto has_target =
          std::any_of(module->functions.begin(), module->functions.end(),
                      [&cfg](const auto &fn) -> auto {
                        return fn != nullptr && fn->name == cfg.run_function;
                      });
      if (has_target) {
        target_module = module.get();
        break;
      }
    }

    if (target_module == nullptr) {
      report.run = run_outcome{
          .succeeded = false,
          .message = std::format("no compiled module defines a function "
                                 "named `{}`",
                                 cfg.run_function)};
    } else {
      report.run =
          run_hir_module(*target_module, checked.types, cfg.run_function);
    }
  }

  if (cfg.build) {
    const auto *target_module = static_cast<const hir::hir_module *>(nullptr);
    for (const auto &module : lowered_modules) {
      if (module == nullptr) {
        continue;
      }
      const auto has_target =
          std::any_of(module->functions.begin(), module->functions.end(),
                      [&cfg](const auto &fn) -> auto {
                        return fn != nullptr && fn->name == cfg.build_function;
                      });
      if (has_target) {
        target_module = module.get();
        break;
      }
    }

    if (target_module == nullptr) {
      report.build = build_outcome{
          .succeeded = false,
          .message = std::format("no compiled module defines a function "
                                 "named `{}`",
                                 cfg.build_function)};
    } else {
      const auto output_path =
          cfg.build_output.empty()
              ? fs::path(source_stem_or_default(fs::path(cfg.sources.front())))
              : fs::path(cfg.build_output);
      report.build =
          build_hir_module(*target_module, checked.types, cfg.build_function,
                           output_path, cfg.program_name);
    }
  }

  return report;
}

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
auto render_compile_summary(const compile_report &report) -> std::string {
  if (report.modules.empty()) {
    return std::format("Compilation failed with {} error(s).",
                       report.error_count);
  }

  auto out = std::format("Compiled {} module(s):", report.modules.size());
  for (size_t i = 0; i < report.modules.size(); ++i) {
    out += std::format("\n  [{}] {} -> {}", i,
                       module_display_name(report.modules[i]),
                       report.modules[i].metadata_path);
  }
  if (!report.hir_modules.empty()) {
    const auto lowered_count = std::count_if(
        report.hir_modules.begin(), report.hir_modules.end(),
        [](const auto &result) -> auto { return result.lowered; });
    out += std::format("\nLowered {}/{} module(s) to HIR.", lowered_count,
                       report.hir_modules.size());
  }
  if (report.run.has_value()) {
    out += report.run->succeeded
               ? std::format("\n{}", report.run->message)
               : std::format("\nrun failed: {}", report.run->message);
  }
  if (report.build.has_value()) {
    out += report.build->succeeded
               ? std::format("\nBuilt executable: {}", report.build->message)
               : std::format("\nbuild failed: {}", report.build->message);
  }
  if (report.error_count > 0) {
    out += std::format("\nEncountered {} error(s).", report.error_count);
  }
  return out;
}

} // namespace kira
