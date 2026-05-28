#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kira {

inline constexpr std::string_view kDefaultMetadataDir = "kira-out/module-metadata";

struct cli_config {
  std::string program_name;
  std::vector<std::string> sources;
  std::string metadata_dir = std::string(kDefaultMetadataDir);
  bool show_help = false;
};

struct compiled_module {
  std::string source_path;
  std::vector<std::string> module_path;
  std::string metadata_path;
};

struct compile_report {
  std::vector<compiled_module> modules;
  std::string diagnostics;
  uint32_t error_count = 0;
};

[[nodiscard]] auto parse_args(int argc, char *argv[])
    -> std::expected<cli_config, std::string>;

[[nodiscard]] auto render_help(std::string_view program_name) -> std::string;

[[nodiscard]] auto compile_sources(const cli_config &cfg,
                                   bool use_color = false)
    -> std::expected<compile_report, std::string>;

[[nodiscard]] auto render_compile_summary(const compile_report &report)
    -> std::string;

} // namespace kira
