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

constexpr std::string_view kMetadataExtension = ".kmeta.pb";
constexpr uint32_t kModuleMetadataSchemaVersion = 1;

auto append_text(std::string &buffer, std::string_view text) -> void {
  if (text.empty()) {
    return;
  }
  if (!buffer.empty() && buffer.back() != '\n') {
    buffer += '\n';
  }
  buffer += text;
}

auto append_error(std::string &buffer, std::string_view message) -> void {
  append_text(buffer, std::format("error: {}", message));
}

[[nodiscard]] auto normalize_path(const fs::path &path) -> std::string {
  return path.lexically_normal().generic_string();
}

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

[[nodiscard]] auto source_stem_or_default(const fs::path &source_path)
    -> std::string {
  auto stem = source_path.stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return "module";
}

[[nodiscard]] auto module_display_name(const compiled_module &module) -> std::string {
  if (!module.module_path.empty()) {
    return join_strings(module.module_path, ".");
  }
  return module.source_path;
}

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

[[nodiscard]] auto unquote_string_literal(std::string_view value) -> std::string {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return std::string(value.substr(1, value.size() - 2));
  }
  return std::string(value);
}

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

auto add_dependency_metadata(const ast::dep_decl &decl,
                             metadata::v1::ModuleMetadata &metadata) -> void {
  auto *dependency = metadata.add_dependencies();
  dependency->set_name(decl.name);
  for (const auto &field : decl.fields) {
    (*dependency->mutable_fields())[field.key] = unquote_string_literal(field.value);
  }
}

auto add_symbol_metadata(const ast::node &node,
                         metadata::v1::ModuleMetadata &metadata) -> void {
  auto *symbol = metadata.add_top_level_symbols();
  symbol->set_name(top_level_name(node));
  symbol->set_kind(symbol_kind_to_proto(node.kind));
  symbol->set_visibility(visibility_to_proto(top_level_visibility(node)));
  symbol->set_has_parse_error(node.has_error);
}

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

struct parsed_input {
  fs::path source_path;
  file_id_type file_id = 0;
  ast::ptr<ast::file> ast_file;
};

struct module_file_record {
  std::string module_name;
  std::string parent_module_name;
  std::string child_name;
  source_location location;
  file_id_type file_id = 0;
  const ast::file *ast_file = nullptr;
};

struct submodule_declaration_record {
  std::string module_name;
  source_location location;
  file_id_type parent_file_id = 0;
  bool is_inline = false;
};

[[nodiscard]] auto module_path_key(const ast::file &file)
    -> std::optional<std::string> {
  if (file.module_decl == nullptr || file.module_decl->has_error ||
      file.module_decl->path.empty()) {
    return std::nullopt;
  }
  return join_strings(file.module_decl->path, ".");
}

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
        .location = source_location{
            .file_id = file_id,
            .span = decl.span,
        },
        .parent_file_id = file_id,
        .is_inline = !decl.items.empty(),
    });

    if (!decl.items.empty()) {
      collect_submodule_declarations(decl.items, module_path, file_id, out);
    }
  }
}

[[nodiscard]] auto has_file_error(const std::vector<bool> &file_has_errors,
                                  file_id_type file_id) -> bool {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return false;
  }
  return file_has_errors[file_id];
}

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

auto mark_file_has_error(std::vector<bool> &file_has_errors,
                         file_id_type file_id) -> void {
  if (static_cast<size_t>(file_id) >= file_has_errors.size()) {
    return;
  }
  file_has_errors[file_id] = true;
}

auto detect_duplicate_module_paths(const std::vector<parsed_input> &inputs,
                                   diagnostic_bag &diag,
                                   std::vector<bool> &file_has_errors) -> void {
  struct seen_module {
    std::string module_name;
    source_location location;
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

auto validate_module_boundaries(const std::vector<parsed_input> &inputs,
                                diagnostic_bag &diag,
                                std::vector<bool> &file_has_errors) -> void {
  const auto module_files = collect_module_files(inputs);
  auto submodule_declarations = std::vector<submodule_declaration_record>{};

  for (const auto &module_file : module_files) {
    if (module_file.ast_file == nullptr || module_file.ast_file->module_decl == nullptr) {
      continue;
    }
    collect_submodule_declarations(module_file.ast_file->items,
                                   module_file.ast_file->module_decl->path,
                                   module_file.file_id, submodule_declarations);
  }

  for (const auto &module_file : module_files) {
    if (module_file.parent_module_name.empty() ||
        has_file_error(file_has_errors, module_file.file_id)) {
      continue;
    }

    const auto *parent =
        find_module_file(module_files, module_file.parent_module_name);
    if (parent == nullptr || has_file_error(file_has_errors, parent->file_id)) {
      continue;
    }

    const auto *declaration = find_submodule_declaration(
        submodule_declarations, parent->file_id, module_file.module_name);
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

} // namespace

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

  detect_duplicate_module_paths(parsed_inputs, session_diagnostics,
                                file_has_errors);
  validate_module_boundaries(parsed_inputs, session_diagnostics, file_has_errors);

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
