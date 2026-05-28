#include "cli.h"

#include <format>

namespace kira {

auto parse_args(int argc, char *argv[]) -> std::expected<cli_config, std::string> {
  if (argc < 1) {
    return std::unexpected{"argc must be at least 1"};
  }

  cli_config cfg{
      .program_name = argv[0],
      .sources = {},
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

    if (parse_options && arg.starts_with('-') && arg != "-") {
      return std::unexpected{std::format("unknown option: {}", arg)};
    }

    cfg.sources.emplace_back(arg);
  }

  return cfg;
}

auto render_help(std::string_view program_name) -> std::string {
  return std::format(
      "Usage: {} [OPTIONS] [SOURCES...]\n\n"
      "Kira - A compiler and language processing tool\n\n"
      "Options:\n"
      "  -h, --help  Show this help message and exit",
      program_name);
}

auto render_source_summary(const cli_config &cfg) -> std::string {
  if (cfg.sources.empty()) {
    return "No source files provided.";
  }

  auto out = std::format("Source file(s) ({}):", cfg.sources.size());
  for (std::size_t i = 0; i < cfg.sources.size(); ++i) {
    out += std::format("\n  [{}] {}", i, cfg.sources[i]);
  }
  return out;
}

} // namespace kira
