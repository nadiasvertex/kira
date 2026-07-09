#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"

namespacekira::driver {

  /// Default output directory for serialized module metadata artifacts.
  inline constexpr std::string_view k_default_metadata_dir =
      "kira-out/module-metadata";

  /// Metadata artifact written for one successfully compiled module file.
  struct compiled_module {
    std::string source_path; ///< Normalized source file path that produced the
                             ///< artifact.
    std::vector<std::string>
        module_path; ///< Canonical module path declared by the source file.
    std::string
        metadata_path; ///< Serialized metadata file emitted for the module.
  };

  /// Render user-facing visibility text for diagnostics.
  ///
  /// @param visibility Visibility modifier attached to a declaration.
  [[nodiscard]] auto visibility_name(ast::visibility visibility)
      -> std::string_view;

  /// Explain how to make an import valid after a visibility failure.
  ///
  /// @param visibility Visibility modifier attached to the imported
  /// declaration.
  /// @param parent_name Parent module that owns the imported child module.
  [[nodiscard]] auto visibility_help(
      ast::visibility visibility, std::string_view parent_name) -> std::string;

  /// Extract the effective top-level visibility from a parsed item node.
  ///
  /// @param node Top-level AST item.
  [[nodiscard]] auto top_level_visibility(const ast::node &node)
      -> ast::visibility;

  /// Extract the user-facing symbol name for a parsed top-level item.
  ///
  /// @param node Top-level AST item.
  [[nodiscard]] auto top_level_name(const ast::node &node) -> std::string;

} // namespace kira::driver
