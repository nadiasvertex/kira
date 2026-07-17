#include "lowering_stage.h"

#include <format>
#include <utility>

#include "src/hir/lower.h"
#include "src/util/path.h"
#include "src/util/str.h"

using kira::util::join_strings;
using kira::util::normalize_path;

namespace kira::driver {

/// Select a fallback filename stem when the real source has no extension.
[[nodiscard]] static auto
source_stem_or_default(const std::filesystem::path &source_path)
    -> std::string {
  auto stem = source_path.stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return "module";
}

auto lower_and_emit_modules(const cli_config &cfg,
                            const std::vector<parsed_input> &parsed_inputs,
                            const std::vector<bool> &file_has_errors,
                            const semantic::checked_types &checked,
                            const std::filesystem::path &metadata_root,
                            compile_report &report)
    -> hir::ptr_vec<hir::hir_module> {
  auto lowered_modules = hir::ptr_vec<hir::hir_module>{};

  for (const auto &input : parsed_inputs) {
    if (static_cast<size_t>(input.file_id) < file_has_errors.size() &&
        file_has_errors[input.file_id]) {
      continue;
    }

    if (input.ast_file == nullptr) {
      continue;
    }

    auto metadata_path = write_module_metadata(
        metadata_root, *input.ast_file, input.source_path, report.diagnostics);
    if (!metadata_path) {
      continue;
    }

    const auto module_path = input.ast_file->module_decl != nullptr
                                 ? input.ast_file->module_decl->path
                                 : std::vector<std::string>{};

    report.modules.push_back(compiled_module{
        .source_path = normalize_path(input.source_path),
        .module_path = module_path,
        .metadata_path = std::move(*metadata_path),
    });

    if (cfg.parse_only) {
      continue;
    }

    auto module_name = join_strings(module_path, ".");
    if (module_name.empty()) {
      module_name = source_stem_or_default(input.source_path);
    }
    auto lowered_result = hir::lower_module(
        *input.ast_file, module_name, checked,
        hir::lowering_options{.contract_checks = cfg.contract_checks});
    report.hir_modules.push_back(
        lowered_result.has_value()
            ? hir_lowering_result{.module_path = module_name, .lowered = true}
            : hir_lowering_result{
                  .module_path = module_name,
                  .lowered = false,
                  .error = std::format("{} (byte offset {})",
                                       lowered_result.error().message,
                                       lowered_result.error().span.start)});
    if (lowered_result.has_value()) {
      lowered_modules.push_back(std::move(*lowered_result));
    }
  }

  // Materialized functor instantiations have no source file the loop above
  // visits — they live only in `checked`. Lower them into standalone modules
  // here and append them, so a `use m[args] as db` program's `db.f(...)`
  // calls resolve to a real compiled module. Skipped under `--parse-only`,
  // which produces no HIR at all.
  if (!cfg.parse_only) {
    auto functor_modules = hir::lower_functor_modules(
        checked, hir::lowering_options{.contract_checks = cfg.contract_checks});
    if (functor_modules.has_value()) {
      for (auto &functor_module : *functor_modules) {
        report.hir_modules.push_back(hir_lowering_result{
            .module_path = functor_module->module_name, .lowered = true});
        lowered_modules.push_back(std::move(functor_module));
      }
    } else {
      report.hir_modules.push_back(hir_lowering_result{
          .module_path = "<functor-instantiation>",
          .lowered = false,
          .error = std::format("{} (byte offset {})",
                               functor_modules.error().message,
                               functor_modules.error().span.start)});
    }
  }

  return lowered_modules;
}

} // namespace kira::driver
