#include "lowering_stage.h"

#include <format>
#include <string>
#include <unordered_set>
#include <utility>

#include "src/hir/frame_budget.h"
#include "src/hir/lower_core.h"
#include "src/runtime/value_layout.h"
#include "src/util/path.h"
#include "src/util/str.h"

using cinder::util::append_text;
using cinder::util::join_strings;
using cinder::util::normalize_path;

namespace cinder::driver {

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

/// Reports a module lowering could not make as an error against the user's
/// source.
///
/// A program that checked can still fail here in two ways. It can use a
/// feature the compiler does not build yet (`core_module_failure::feature`):
/// the message says which, and the user can avoid it. Or lowering refused a
/// construct it should handle: an internal compiler error, which names the
/// construct and says it is a bug in the compiler, not in the program.
///
/// Reported only when code was actually asked for (`--run`/`--compile`).
/// That is the case where the failure is load-bearing: the user wanted
/// something to execute, this module produced none, and the next thing they
/// would otherwise see is `no compiled module defines a function named
/// `main``, which names neither the real cause nor the real location. A
/// metadata-or-analysis run has no such gap to explain, so it stays quiet,
/// with the detail still available in `report.hir_modules` under
/// `--show-compile-details`.
static auto report_lowering_failure(const cli_config &cfg,
                                    const hir::core_module_failure &failure,
                                    file_id_type file_id,
                                    const diagnostic_renderer &renderer,
                                    compile_report &report) -> void {
  if (!cfg.run && !cfg.build) {
    return;
  }
  if (failure.feature) {
    auto diag = diagnostic(diagnostic_level::error,
                           std::format("`{}` uses a feature the compiler "
                                       "does not build yet",
                                       failure.item),
                           file_id);
    diag.with_label(failure.span, failure.message);
    diag.with_note("the program is valid; the compiler cannot generate code "
                   "for this construct yet");
    append_text(report.diagnostics, renderer.render(diag));
    ++report.error_count;
    return;
  }
  auto diag = diagnostic(
      diagnostic_level::error,
      std::format("internal compiler error: could not lower `{}` in module "
                  "`{}`",
                  failure.item, failure.module),
      file_id);
  diag.with_label(failure.span,
                  std::format("lowering refused this: {}", failure.message));
  diag.with_note("this is a bug in the compiler, not in your program; "
                 "please report it with this program");
  append_text(report.diagnostics, renderer.render(diag));
  ++report.error_count;
}

/// Renders a byte count the way a person would say it: `1 MiB`, `96 KiB`,
/// or plain bytes when it is not a whole number of either.
[[nodiscard]] static auto format_bytes(uint64_t bytes) -> std::string {
  constexpr auto kib = uint64_t{1} << 10;
  constexpr auto mib = uint64_t{1} << 20;
  if (bytes != 0 && bytes % mib == 0) {
    return std::format("{} MiB", bytes / mib);
  }
  if (bytes != 0 && bytes % kib == 0) {
    return std::format("{} KiB", bytes / kib);
  }
  return std::format("{} bytes", bytes);
}

/// Reports every frame in `module` whose `uninit` buffers exceed
/// `hir::k_max_frame_stack_bytes`. Unlike a lowering failure this is the
/// program's error, not the compiler's, and it is reported whether or not
/// code was asked for: the rule is part of the language, not of whichever
/// backend runs next. Returns whether `module` is clean.
static auto check_frame_budgets(hir::hir_module &module,
                                runtime::value_layouts &layouts,
                                file_id_type file_id,
                                const diagnostic_renderer &renderer,
                                compile_report &report) -> bool {
  const auto violations = hir::find_frame_budget_violations(module, layouts);
  for (const auto &violation : violations) {
    auto diag = diagnostic(
        diagnostic_level::error,
        std::format("{} needs {} of `uninit` stack storage, more than the {} "
                    "one function frame may use",
                    violation.frame_name, format_bytes(violation.total_bytes),
                    format_bytes(hir::k_max_frame_stack_bytes)),
        file_id);
    for (const auto &buffer : violation.buffers) {
      diag.with_label(buffer.span, std::format("this buffer takes {}",
                                               format_bytes(buffer.byte_size)));
    }
    diag.with_note(std::format(
        "`uninit[T, N]` lives in the function's stack frame, and a frame this "
        "large can overflow the thread's stack. Every `uninit` buffer in one "
        "function counts toward the same {} limit, on every backend.",
        format_bytes(hir::k_max_frame_stack_bytes)));
    diag.with_help("store this many elements on the heap instead (`list[T]` "
                   "or `vec[T]`), or use a smaller `N`");
    append_text(report.diagnostics, renderer.render(diag));
    ++report.error_count;
  }
  return violations.empty();
}

auto hir_module_name(const parsed_input &input) -> std::string {
  const auto module_path = input.ast_file != nullptr &&
                                   input.ast_file->module_decl != nullptr
                               ? input.ast_file->module_decl->path
                               : std::vector<std::string>{};
  auto name = join_strings(module_path, ".");
  if (name.empty()) {
    name = source_stem_or_default(input.source_path);
  }
  return name;
}

/// Takes the modules lowering made and reports each one it could not make.
static auto take_modules(const cli_config &cfg,
                         const std::vector<parsed_input> &parsed_inputs,
                         hir::core_program_lowering &lowering,
                         runtime::value_layouts &layouts,
                         const diagnostic_renderer &renderer,
                         compile_report &report)
    -> hir::ptr_vec<hir::hir_module> {
  auto lowered_modules = hir::ptr_vec<hir::hir_module>{};
  const auto fallback_file = parsed_inputs.empty()
                                 ? file_id_type{0}
                                 : parsed_inputs.front().file_id;
  // One report per module: its first failure.
  auto reported = std::unordered_set<std::string>{};
  for (const auto &failure : lowering.failures) {
    if (!reported.insert(failure.module).second) {
      continue;
    }
    report.hir_modules.push_back(hir_lowering_result{
        .module_path = failure.module,
        .lowered = false,
        .error = std::format("{} (byte offset {})", failure.message,
                             failure.span.start)});
    report_lowering_failure(cfg, failure,
                            failure.file_id.value_or(fallback_file), renderer,
                            report);
  }
  // A submodule's spans are in its parent's file.
  auto file = fallback_file;
  for (auto &module : lowering.modules) {
    if (module->file_id.has_value()) {
      file = *module->file_id;
    }
    report.hir_modules.push_back(hir_lowering_result{
        .module_path = module->module_name, .lowered = true, .error = {}});
    if (check_frame_budgets(*module, layouts, file, renderer, report)) {
      lowered_modules.push_back(std::move(module));
    }
  }
  lowering.modules.clear();
  return lowered_modules;
}

auto lower_and_emit_modules(const cli_config &cfg,
                            const std::vector<parsed_input> &parsed_inputs,
                            const std::vector<bool> &file_has_errors,
                            const semantic::checked_types &checked,
                            const std::filesystem::path &metadata_root,
                            const diagnostic_renderer &renderer,
                            compile_report &report,
                            hir::core_program_lowering *lowering)
    -> hir::ptr_vec<hir::hir_module> {
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
  }

  if (lowering == nullptr || cfg.parse_only) {
    return {};
  }
  // Checking is over, so every instance's shape is settled.
  auto layouts = runtime::value_layouts(checked.types, checked.shapes);
  return take_modules(cfg, parsed_inputs, *lowering, layouts, renderer,
                      report);
}

} // namespace cinder::driver
