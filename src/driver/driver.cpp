#include "driver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <system_error>

#include "src/hir/lower.h"
#include "src/parser/diagnostic.h"
#include "src/parser/parser.h"
#include "src/parser/source_location.h"
#include "src/semantic/analysis.h"
#include "src/util/path.h"
#include "src/util/str.h"

using kira::lexer;
using kira::source_manager;
using kira::util::append_error;
using kira::util::append_text;
using kira::util::join_strings;
using kira::util::normalize_path;
using kira::util::read_source_file;

namespace kira::driver {

/// Parsed source file plus the bookkeeping needed by later driver passes.
struct parsed_input {
  std::filesystem::path source_path;
  file_id_type file_id = 0;
  ast::ptr<ast::file> ast_file;
};

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
static auto first_source_stem(auto &&cfg) -> std::string {
  auto stem = std::filesystem::path(cfg.sources.front()).stem().string();
  if (!stem.empty()) {
    return stem;
  }
  return "kira";
}

/// Locates `filename` under the `src/std` Bazel package, trying every
/// invocation shape the binary might run under (`bazelisk run`'s runfiles
/// tree, `bazel test`'s `TEST_SRCDIR`, or a plain `bazel-bin` invocation
/// from the workspace root) — the same set of candidates
/// `find_bazel_archive` (`src/driver/aot.cpp`) tries for the AOT runtime
/// archives.
[[nodiscard]] static auto
find_stdlib_source_file(std::string_view program_name,
                        std::string_view filename)
    -> std::optional<std::filesystem::path> {
  auto candidates = std::vector<std::filesystem::path>{};
  if (!program_name.empty()) {
    candidates.emplace_back(std::filesystem::path(
                                std::format("{}.runfiles", program_name)) /
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
    return std::ranges::any_of(
        cfg.sources, [&](const auto &source) -> bool {
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
  auto parsed_inputs = std::vector<parsed_input>{};

  for (const auto &source_arg : cfg.sources) {
    const auto source_path = std::filesystem::path(source_arg);
    auto source_text = read_source_file(source_path);
    if (!source_text) {
      append_error(report.diagnostics, source_text.error());
      continue;
    }

    auto file_id =
        sources.add_file(normalize_path(source_path), std::move(*source_text));
    if (!file_id) {
      append_error(report.diagnostics, file_id.error());
      continue;
    }

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    if (file == nullptr) {
      append_error(report.diagnostics, "internal error: missing source file");
      continue;
    }

    auto errors_before = session_diagnostics.error_count();
    auto tokenizer = lexer(file->source(), file->id(), session_diagnostics);
    auto tokens = tokenizer.tokenize();
    auto parser_instance =
        parser(std::move(tokens), file->id(), session_diagnostics);
    auto ast_file = parser_instance.parse_file();

    if (session_diagnostics.error_count() > errors_before) {
      file_has_errors[*file_id] = true;
    }

    parsed_inputs.push_back(parsed_input{
        .source_path = source_path,
        .file_id = *file_id,
        .ast_file = std::move(ast_file),
    });
  }

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

    if (!cfg.parse_only) {
      auto module_name = join_strings(module_path, ".");
      if (module_name.empty()) {
        module_name = source_stem_or_default(input.source_path);
      }
      auto lowered_result =
          hir::lower_module(*input.ast_file, module_name, checked);
      report.hir_modules.push_back(
          lowered_result.has_value()
              ? hir_lowering_result{.module_path = module_name, .lowered = true}
              : hir_lowering_result{.module_path = module_name,
                                    .lowered = false,
                                    .error = lowered_result.error().message});
      if (lowered_result.has_value()) {
        lowered_modules.push_back(std::move(*lowered_result));
      }
    }
  }

  if (cfg.run) {
    const auto *target_module =
        find_module_by_function(lowered_modules, cfg.run_function);

    if (target_module == nullptr) {
      report.run = run_outcome{
          .succeeded = false,
          .message = std::format("no compiled module defines a function "
                                 "named `{}`",
                                 cfg.run_function)};
    } else {
      report.run =
          run_hir_module(*target_module, checked.types, cfg.run_function);
    }
  }

  if (cfg.build) {
    const auto *target_module =
        find_module_by_function(lowered_modules, cfg.build_function);

    if (target_module == nullptr) {
      report.build = build_outcome{
          .succeeded = false,
          .message = std::format("no compiled module defines a function "
                                 "named `{}`",
                                 cfg.build_function)};
    } else {
      const auto output_path =
          cfg.build_output.empty()
              ? std::filesystem::path(std::string(first_source_stem(cfg)) +
                                      ".out")
              : std::filesystem::path(cfg.build_output);
      report.build =
          build_hir_module(*target_module, checked.types, cfg.build_function,
                           output_path, cfg.program_name);
    }
  }

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
