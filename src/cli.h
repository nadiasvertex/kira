#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kira {

struct cli_config {
  std::string_view program_name;
  std::vector<std::string> sources;
  bool show_help = false;
};

[[nodiscard]] auto parse_args(int argc, char *argv[])
    -> std::expected<cli_config, std::string>;

[[nodiscard]] auto render_help(std::string_view program_name) -> std::string;

[[nodiscard]] auto render_source_summary(const cli_config &cfg) -> std::string;

} // namespace kira
