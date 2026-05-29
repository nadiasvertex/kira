#include <cstdio>
#include <print>

#include <unistd.h>

#include "cli.h"

/// Run the Kira CLI driver from process entry to exit status.
///
/// @param argc Argument count supplied by the host process.
/// @param argv Argument vector supplied by the host process.
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

  auto report = kira::compile_sources(cfg, ::isatty(fileno(stderr)) != 0);
  if (!report) {
    std::println(stderr, "Error: {}", report.error());
    return 1;
  }

  if (!report->diagnostics.empty()) {
    std::print(stderr, "{}", report->diagnostics);
    if (!report->diagnostics.ends_with('\n')) {
      std::println(stderr);
    }
  }

  std::println("{}", kira::render_compile_summary(*report));
  return report->error_count == 0 ? 0 : 1;
}
