#include "driver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>

#include "lowering_stage.h"
#include "parse_stage.h"
#include "run_build_stage.h"
#include "src/semantic/analysis.h"
#include "src/util/path.h"
#include "src/util/str.h"

using kira::source_manager;
using kira::util::append_text;
using kira::util::normalize_path;

namespace kira::driver {

/// Locates `filename` under the `src/std` Bazel package, trying every
/// invocation shape the binary might run under (`bazelisk run`'s runfiles
/// tree, `bazel test`'s `TEST_SRCDIR`, or a plain `bazel-bin` invocation
/// from the workspace root) — the same set of candidates
/// `find_bazel_archive` (`src/driver/aot.cpp`) tries for the AOT runtime
/// archives.
[[nodiscard]] static auto find_stdlib_source_file(std::string_view program_name,
                                                  std::string_view filename)
    -> std::optional<std::filesystem::path> {
  auto candidates = std::vector<std::filesystem::path>{};
  if (!program_name.empty()) {
    candidates.emplace_back(
        std::filesystem::path(std::format("{}.runfiles", program_name)) /
        "_main" / "src" / "std" / filename);
  }
  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(std::filesystem::path(srcdir) / workspace /
                              "src" / "std" / filename);
    }
    candidates.emplace_back(std::filesystem::path(srcdir) / "_main" / "src" /
                            "std" / filename);
  }
  candidates.emplace_back(std::filesystem::path("src") / "std" / filename);

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (std::filesystem::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto inject_stdlib_prelude(cli_config &cfg) -> void {
  auto already_present =
      [&cfg](const std::filesystem::path &candidate) -> bool {
    const auto normalized = normalize_path(candidate);
    return std::ranges::any_of(cfg.sources, [&](const auto &source) -> bool {
      return normalize_path(std::filesystem::path(source)) == normalized;
    });
  };

  // `traits.kira` first: it declares the traits `prelude.kira` depends on,
  // though declaration order across session files has no effect on
  // resolution — the whole session's module index is built before any file
  // is checked.
  for (const auto *filename : {"traits.kira", "prelude.kira"}) {
    const auto found = find_stdlib_source_file(cfg.program_name, filename);
    if (found && !already_present(*found)) {
      cfg.sources.push_back(found->string());
    }
  }
}

auto compile_sources(const cli_config &cfg, bool use_color)
    -> std::expected<compile_report, std::string> {
  if (cfg.sources.empty()) {
    return std::unexpected{"no source files provided"};
  }

  auto report = compile_report{};
  const auto metadata_root = std::filesystem::path(cfg.metadata_dir);
  auto sources = source_manager{};
  auto session_diagnostics = diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};

  const auto parsed_inputs = parse_sources(cfg, sources, session_diagnostics,
                                           file_has_errors, report.diagnostics);

  auto semantic_inputs = std::vector<semantic::parsed_module>{};
  semantic_inputs.reserve(parsed_inputs.size());
  for (const auto &input : parsed_inputs) {
    semantic_inputs.push_back(semantic::parsed_module{
        .file_id = input.file_id,
        .ast_file = input.ast_file.get(),
    });
  }

  const auto checked = semantic::validate_semantics(
      semantic_inputs, session_diagnostics, file_has_errors,
      semantic::semantic_options{.check_names_and_types = !cfg.parse_only});

  report.error_count += session_diagnostics.error_count();
  append_text(
      report.diagnostics,
      diagnostic_renderer(sources, use_color).render_all(session_diagnostics));

  const auto lowered_modules = lower_and_emit_modules(
      cfg, parsed_inputs, file_has_errors, checked, metadata_root, report);

  run_requested_function(cfg, lowered_modules, checked, report);
  build_requested_function(cfg, lowered_modules, checked, report);

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
    if (i < report.modules.size()) {
      out += std::format("\n  [{}] {} -> {}", i,
                         module_display_name(report.modules[i]),
                         report.modules[i].metadata_path);
    }
  }
  if (!report.hir_modules.empty()) {
    const auto lowered_count = static_cast<size_t>(
        std::count_if(report.hir_modules.begin(), report.hir_modules.end(),
                      [](const auto &result) { return result.lowered; }));
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

} // namespace kira::driver
