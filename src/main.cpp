#include <print>

#include "cli.h"

auto main(int argc, char *argv[]) -> int {
  auto result = kira::parse_args(argc, argv);

  if (!result) {
    std::println(stderr, "Error: {}", result.error());
    return 1;
  }

  const auto &cfg = *result;

  if (cfg.show_help) {
    std::println("{}", kira::render_help(cfg.program_name));
    return 0;
  }

  std::println("{}", kira::render_source_summary(cfg));
  return 0;
}
