#include "driver.h"
#include "src/util/str.h"

using kiri::util::append_text;

namespace kiri::driver {

auto compile_sources(const cli_config &cfg, bool /*use_color*/)
    -> std::expected<compile_report, std::string> {
  if (cfg.sources.empty()) {
    return std::unexpected{"no source files provided"};
  }

  auto report = compile_report{};
  append_text(
      report.diagnostics,
      std::format("compile_sources: found {} source(s)", cfg.sources.size()));
  return report;
}

auto render_compile_summary(const compile_report &report) -> std::string {
  if (report.modules.empty()) {
    return std::format("Compilation failed with {} error(s).",
                       report.error_count);
  }

  std::string out =
      std::format("Compiled {} module(s):", report.modules.size());
  for (size_t i = 0; i < report.modules.size(); ++i) {
    out += std::format("\n  [{}] {} -> {}", i, "N/A",
                       report.modules[i].metadata_path);
  }
  if (!report.hir_modules.empty()) {
    size_t lowered_count = 0;
    for (const auto &result : report.hir_modules) {
      if (result.lowered) {
        ++lowered_count;
      }
    }
    out += std::format("\nLowered {}/{} module(s) to HIR.", lowered_count,
                       report.hir_modules.size());
  }
  if (report.run.has_value()) {
    out += report.run->succeeded
               ? std::format("\n{}", report.run->message)
               : std::format("\nrun failed: {}", report.run->message);
  }
  if (report.build.has_value()) {
    out += report.build->succeeded
               ? std::format("\nBuilt executable: {}", report.build->message)
               : std::format("\nbuild failed: {}", report.build->message);
  }
  if (report.error_count > 0) {
    out += std::format("\nEncountered {} error(s).", report.error_count);
  }
  return out;
}

} // namespace kiri::driver
