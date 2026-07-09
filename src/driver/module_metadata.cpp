#include <filesystem>
#include <string_view>

#include "module_metadata.h"
#include "src/module_metadata.pb.h"

namespace kira::driver {

namespace {

namespace fs = std::filesystem;

/// File extension used for serialized module metadata artifacts.
constexpr std::string_view k_metadata_extension = ".kmeta";

/// Schema version embedded in every emitted module metadata payload.
constexpr uint32_t k_module_metadata_schema_version = 1;

} // namespace

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
  case ast::use_selector_kind::single:
    return metadata::v1::IMPORT_SELECTOR_KIND_SINGLE;
  case ast::use_selector_kind::group:
    return metadata::v1::IMPORT_SELECTOR_KIND_GROUP;
  case ast::use_selector_kind::wildcard:
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

} // namespace kira::driver
