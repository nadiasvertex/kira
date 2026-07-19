#include "lowering_stage.h"

#include <format>
#include <utility>

#include "src/hir/lower.h"
#include "src/util/path.h"
#include "src/util/str.h"

using kira::util::append_text;
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

/// Reports a lowering failure as a real error against the user's source.
///
/// Lowering is the last phase that can reject a program, and the only one
/// whose failure the user cannot see coming: the code type-checked, so as far
/// as they know it is correct. Saying which declaration could not be emitted —
/// and that the gap is the compiler's, not theirs — is the difference between
/// a bug report and a workaround.
///
/// Reported only when code was actually asked for (`--run`/`--compile`).
/// That is the case where the failure is load-bearing: the user wanted
/// something to execute, this module produced none, and the next thing they
/// would otherwise see is `no compiled module defines a function named
/// `main``, which names neither the real cause nor the real location. A
/// metadata-or-analysis run has no such gap to explain — plenty of modules
/// legitimately lower to nothing there — so it stays quiet, with the detail
/// still available in `report.hir_modules` under `--show-compile-details`.
static auto report_lowering_failure(const cli_config &cfg,
                                    const hir::lowering_error &error,
                                    std::string_view module_name,
                                    file_id_type file_id,
                                    const diagnostic_renderer &renderer,
                                    compile_report &report) -> void {
  if (!cfg.run && !cfg.build) {
    return;
  }
  auto diag =
      diagnostic(diagnostic_level::error,
                 std::format("could not lower module `{}` to executable form",
                             module_name),
                 file_id);
  diag.with_label(error.span, error.message);
  diag.with_note("this module type-checked; the failure is in the compiler's "
                 "code-generation phase, not in your program");
  diag.with_help("this is a gap in the compiler, not a mistake in your code. "
                 "Rewriting the highlighted construct in a simpler form is "
                 "the available workaround, and the gap is worth reporting.");
  append_text(report.diagnostics, renderer.render(diag));
  ++report.error_count;
}

auto lower_and_emit_modules(const cli_config &cfg,
                            const std::vector<parsed_input> &parsed_inputs,
                            const std::vector<bool> &file_has_errors,
                            const semantic::checked_types &checked,
                            const std::filesystem::path &metadata_root,
                            const diagnostic_renderer &renderer,
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
    } else {
      report_lowering_failure(cfg, lowered_result.error(), module_name,
                              input.file_id, renderer, report);
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
      // The instantiated `def`s were cloned from some real source file, so the
      // error's span still points into one — take the first input's file for
      // rendering, which is where a single-file program's functor lives.
      report_lowering_failure(
          cfg, functor_modules.error(), "<functor-instantiation>",
          parsed_inputs.empty() ? file_id_type{0}
                                : parsed_inputs.front().file_id,
          renderer, report);
    }
  }

  return lowered_modules;
}

} // namespace kira::driver
