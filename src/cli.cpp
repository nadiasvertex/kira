#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "driver/driver.h"
#include "driver/interpret.h"
#include "driver/aot.h"

#include <filesystem>

#include "src/bytecode/panic.h"
#include "src/bytecode/value.h"
#include "src/bytecode/vm.h"
#include "src/bytecode_compiler/compile.h"
#include "src/hir/lower.h"
#include "src/llvm_codegen/aot.h"
#include "src/llvm_codegen/codegen.h"
#include "src/module_metadata.pb.h"
#include "src/semantic/analysis.h"
#include "src/semantic/types.h"
#include "src/util/path.h"
#include "src/util/str.h"

namespace fs = std::filesystem;

namespace {

/// Parse CLI arguments into the compile driver's configuration structure.
///
/// @param argc Argument count passed to `main`.
/// @param argv Argument vector passed to `main`.
auto parse_args(std::span<char *const> argv)
    -> std::expected<cli_config, std::string> {
  if (argv.empty()) {
    return std::unexpected{"argc must be at least 1"};
  }

  auto cfg = cli_config{
      .program_name = argv[0],
      .sources = {},
      .metadata_dir = std::string(k_default_metadata_dir),
      .show_help = false,
  };

  // Track whether the `--` end-of-options marker has been seen.
  bool parse_options = true;
  for (size_t i = 1; i < argv.size(); ++i) {
    std::string_view arg = argv[i];

    if (!parse_options) {
      cfg.sources.emplace_back(arg);
      continue;
    }

    if (arg == "--") {
      parse_options = false;
      continue;
    }

    bool matched_flag = false;
    std::string_view value{};
    const cli_converter *matched_converter = nullptr;
    if (auto it = k_flag_converters.find(arg); it != k_flag_converters.end()) {
      matched_flag = true;
      matched_converter = &it->second;
      if (it->second.needs_value) {
        if (i + 1 >= argv.size()) {
          return std::unexpected{
              std::format("missing value for `{}`", std::string{arg})};
        }
        value = argv[i + 1];
        ++i;
      }
    } else {
      // --flag=value form.
      if (auto eq_pos = arg.find('='); eq_pos != std::string_view::npos) {
        std::string_view flag_key = arg.substr(0, eq_pos);
        if (auto eq_it = k_flag_converters.find(flag_key);
            eq_it != k_flag_converters.end() && eq_it->second.needs_value) {
          matched_flag = true;
          matched_converter = &eq_it->second;
          value = arg.substr(eq_pos + 1);
        }
      }
    }
    if (matched_flag) {
      auto r = matched_converter->apply(cfg, value);
      if (!r) {
        return std::unexpected{r.error()};
      }
      continue;
    }

    if (arg.starts_with('-') && arg != "-") {
      return std::unexpected{std::format("unknown option: {}", arg)};
    }

    cfg.sources.emplace_back(arg);
  }

  // Running is the default mode: `kira SOURCE` alone executes it via the
  // tier-0 VM without needing an explicit `--run`. `--compile` opts into
  // AOT compilation instead; pair it with an explicit `--run`/`--run-function`
  // to do both.
  if (!cfg.build && !cfg.run) {
    cfg.run = true;
  }

  return cfg;
}

/// Render the user-facing CLI help text.
///
/// @param program_name Executable name to display in the usage line.
auto render_help(std::string_view program_name) -> std::string {
  return std::format(
      "Usage: {} [OPTIONS] SOURCES...\n\n"
      "Kira - Parse source files and emit module metadata\n\n"
      "Options:\n"
      "  -h, --help               Show this help message and exit\n"
      "  --metadata-dir PATH      Write module metadata under PATH\n"
      "                          (default: {})\n"
      "  (default)                Compile to bytecode and execute `{}` via\n"
      "                          the tier-0 VM (increment 1's scalar/\n"
      "                          control-flow subset only; see\n"
      "                          src/bytecode_compiler) — no flag needed\n"
      "  --run-function NAME      Like the default run, but execute NAME\n"
      "                          instead of `{}`\n"
      "  --compile                Compile `{}` to native code via LLVM, link\n"
      "                          a standalone executable instead of running\n"
      "                          it (same scalar/control-flow subset; see\n"
      "                          src/llvm_codegen)\n"
      "  --compile-function NAME  Like --compile, but use NAME as the entry\n"
      "                          point\n"
      "  --compile-output PATH    Write the linked executable to PATH\n"
      "  --parse-only             Run only the lexer and parser,\n"
      "                          skipping semantic name resolution and\n"
      "                          type checking",
      program_name, k_default_metadata_dir, k_default_run_function,
      k_default_run_function, k_default_run_function);
}

/// Parse, validate, and emit metadata for each requested source file.
///
/// @param cfg Command-line configuration that names source inputs and outputs.
/// @param use_color Whether rendered diagnostics should include ANSI colors.
auto compile_sources(const cli_config &cfg, bool use_color)
    -> std::expected<compile_report, std::string> {
  if (cfg.sources.empty()) {
    return std::unexpected{"no source files provided"};
  }

  auto report = compile_report{};
  const auto metadata_root = fs::path(cfg.metadata_dir);
  auto sources = source_manager{};
  auto session_diagnostics = diagnostic_bag{};
  auto file_has_errors = std::vector<bool>{};
  auto parsed_inputs = std::vector<parsed_input>{};

  for (const auto &source_arg : cfg.sources) {
    const auto source_path = fs::path(source_arg);
    auto source_text = read_source_file(source_path);
    if (!source_text) {
      ++report.error_count;
      append_error(report.diagnostics, source_text.error());
      continue;
    }

    auto file_id =
        sources.add_file(normalize_path(source_path), std::move(*source_text));
    if (!file_id) {
      ++report.error_count;
      append_error(report.diagnostics, file_id.error());
      continue;
    }

    if (file_has_errors.size() <= static_cast<size_t>(*file_id)) {
      file_has_errors.resize(static_cast<size_t>(*file_id) + 1, false);
    }

    const auto *file = sources.get(*file_id);
    if (file == nullptr) {
      ++report.error_count;
      append_error(report.diagnostics, "internal error: missing source file");
      continue;
    }

    auto errors_before = session_diagnostics.error_count();
    auto tokenizer = lexer(file->source(), file->id(), session_diagnostics);
    auto tokens = tokenizer.tokenize();
    auto parser =
        kira::parser(std::move(tokens), file->id(), session_diagnostics);
    auto ast_file = parser.parse_file();

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
  // Consumed below to best-effort lower each successfully-checked module to
  // HIR (src/hir/lower.h); meaningless (empty) when cfg.parse_only skipped
  // checking, so lowering is skipped in that case too.
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
      ++report.error_count;
      continue;
    }

    const auto module_path = input.ast_file->module_decl != nullptr
                                 ? input.ast_file->module_decl->path
                                 : std::vector<std::string>{};
    const auto module_name = join_strings(module_path, ".");

    report.modules.push_back(compiled_module{
        .source_path = normalize_path(input.source_path),
        .module_path = module_path,
        .metadata_path = std::move(*metadata_path),
    });

    // Best-effort only (see hir_lowering_result's doc comment): lowering
    // coverage is still partial, so a failure here is not a compile error.
    if (!cfg.parse_only) {
      auto lowered = hir::lower_module(*input.ast_file, module_name, checked);
      report.hir_modules.push_back(
          lowered.has_value()
              ? hir_lowering_result{.module_path = module_name, .lowered = true}
              : hir_lowering_result{.module_path = module_name,
                                    .lowered = false,
                                    .error = lowered.error().message});
      if (lowered.has_value()) {
        lowered_modules.push_back(std::move(*lowered));
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
              ? fs::path(source_stem_or_default(fs::path(cfg.sources.front())))
              : fs::path(cfg.build_output);
      report.build =
          build_hir_module(*target_module, checked.types, cfg.build_function,
                           output_path, cfg.program_name);
    }
  }

  return report;
}

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
auto render_compile_summary(const compile_report &report) -> std::string {
  if (report.modules.empty()) {
    return std::format("Compilation failed with {} error(s).",
                       report.error_count);
  }

  auto out = std::format("Compiled {} module(s):", report.modules.size());
  for (size_t i = 0; i < report.modules.size(); ++i) {
    out += std::format("\n  [{}] {} -> {}", i,
                       module_display_name(report.modules[i]),
                       report.modules[i].metadata_path);
  }
  if (!report.hir_modules.empty()) {
    const auto lowered_count = std::count_if(
        report.hir_modules.begin(), report.hir_modules.end(),
        [](const auto &result) -> auto { return result.lowered; });
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

} // namespace kira
