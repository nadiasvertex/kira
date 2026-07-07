#include <cstdio>
#include <exception>
#include <print>
#include <span>

#include <unistd.h>

#include "cli.h"

/// Run the Kira CLI driver from process entry to exit status.
///
/// @param argc Argument count supplied by the host process.
/// @param argv Argument vector supplied by the host process.
auto main(int argc, char *argv[]) -> int {
  try {
    auto result = kira::parse_args(
        std::span<char *const>(argv, static_cast<size_t>(argc)));

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
    const auto run_failed = report->run.has_value() && !report->run->succeeded;
    const auto build_failed =
        report->build.has_value() && !report->build->succeeded;
    return report->error_count == 0 && !run_failed && !build_failed ? 0 : 1;
  } catch (const std::exception &ex) {
    // Avoid std::format/std::println here: they can themselves throw, and a
    // throw from within this handler would escape main() uncaught.
    std::fputs("Error: unhandled exception: ", stderr);
    std::fputs(ex.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }
}
