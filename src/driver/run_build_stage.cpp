#include "run_build_stage.h"

#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "src/hir/link.h"

namespace kira::driver {

/// Find the first lowered HIR module that defines a function matching the
/// given name.  Returns nullptr when no match is found.
[[nodiscard]] static auto
find_module_by_function(const hir::ptr_vec<hir::hir_module> &modules,
                        std::string_view function_name)
    -> const hir::hir_module * {
  for (const auto &module : modules) {
    if (module == nullptr) {
      continue;
    }
    for (const auto &fn : module->functions) {
      if (fn != nullptr && fn->name == function_name) {
        return module.get();
      }
    }
  }
  return nullptr;
}

// Obtain a fallback stem for the first source file (used by build path).
static auto first_source_stem(const cli_config &cfg) -> std::string {
  auto stem = std::filesystem::path(cfg.sources.front()).stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return "kira";
}

auto run_requested_function(
    const cli_config &cfg, const hir::ptr_vec<hir::hir_module> &lowered_modules,
    const semantic::checked_types &checked, compile_report &report) -> void {
  if (!cfg.run) {
    return;
  }

  const auto *target_module =
      find_module_by_function(lowered_modules, cfg.run_function);

  if (target_module == nullptr) {
    report.run =
        run_outcome{.succeeded = false,
                    .message = std::format(
                        "no compiled module defines a function named `{}`",
                        cfg.run_function)};
    return;
  }

  const auto reachable =
      hir::find_reachable_modules(*target_module, lowered_modules);
  report.run = run_hir_module(reachable, checked.types, cfg.run_function);
}

auto build_requested_function(
    const cli_config &cfg, const hir::ptr_vec<hir::hir_module> &lowered_modules,
    const semantic::checked_types &checked, compile_report &report) -> void {
  if (!cfg.build) {
    return;
  }

  const auto *target_module =
      find_module_by_function(lowered_modules, cfg.build_function);

  if (target_module == nullptr) {
    report.build =
        build_outcome{.succeeded = false,
                      .message = std::format(
                          "no compiled module defines a function named `{}`",
                          cfg.build_function)};
    return;
  }

  const auto reachable =
      hir::find_reachable_modules(*target_module, lowered_modules);
  const auto output_path =
      cfg.build_output.empty()
          ? std::filesystem::path(first_source_stem(cfg) + ".out")
          : std::filesystem::path(cfg.build_output);
  report.build = build_hir_module(reachable, checked.types, cfg.build_function,
                                  output_path, cfg.program_name, cfg.opt_level);
}

} // namespace kira::driver
