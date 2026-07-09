#pragma once

#include <string>
#include <vector>

namespace kira::driver {

/// Default output directory for serialized module metadata artifacts.
inline constexpr std::string_view k_default_metadata_dir =
    "kira-out/module-metadata";

/// Metadata artifact written for one successfully compiled module file.
struct compiled_module {
  std::string
      source_path; ///< Normalized source file path that produced the artifact.
  std::vector<std::string>
      module_path; ///< Canonical module path declared by the source file.
  std::string
      metadata_path; ///< Serialized metadata file emitted for the module.
};
} // namespace kira::driver
