#include <cstdio>
#include <exception>
#include <print>
#include <span>

#include <unistd.h>

#include "driver/cli.h"
#include "version.h"

/// Run the Kira CLI driver from process entry to exit status.
///
/// @param argc Argument count supplied by the host process.
/// @param argv Argument vector supplied by the host process.
auto main(int argc, char *argv[]) -> int {
  try {
    auto result = kira::driver::parse_args(
        std::span<char *const>(argv, static_cast<size_t>(argc)));

    if (!result) {
      std::println(stderr, "Error: {}", result.error());
      return 1;
    }

    auto cfg = *result;

    if (cfg.show_version) {
      std::println("kira {}", kira::k_version_string);
      return 0;
    }

    if (cfg.show_help) {
      std::println("{}", kira::driver::render_help(cfg.program_name));
      return 0;
    }

    kira::driver::inject_stdlib_prelude(cfg);

    auto report =
        kira::driver::compile_sources(cfg, ::isatty(fileno(stderr)) != 0);
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

    const auto run_failed = report->run.has_value() && !report->run->succeeded;
    const auto build_failed =
        report->build.has_value() && !report->build->succeeded;
    const auto had_errors =
        report->error_count > 0 || run_failed || build_failed;

    const auto summary =
        kira::driver::render_compile_summary(*report, cfg.show_compile_details);
    if (!summary.empty()) {
      std::println("{}", summary);
    }

    // In interpreter mode the process exit code mirrors whatever `main`
    // (or `--run-function`'s target) returned, like any other program —
    // not the usual "0 success / 1 failure" the compiler itself uses for
    // every other outcome (`spec/codegen-design.md`'s driver semantics).
    if (cfg.run && !had_errors) {
      return report->run.has_value() ? report->run->exit_code : 0;
    }
    return had_errors ? 1 : 0;
  } catch (const std::exception &ex) {
    // Avoid std::format/std::println here: they can themselves throw, and a
    // throw from within this handler would escape main() uncaught.
    std::fputs("Error: unhandled exception: ", stderr);
    std::fputs(ex.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }
}
