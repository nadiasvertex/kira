#include <expected>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "defaults.h"
#include "driver.h"
#include "module_metadata.h"
#include "src/parser/parser.h"
#include "src/util/path.h"
#include "src/util/str.h"

namespace kira::driver {

struct cli_converter {
  std::string_view name;

  bool needs_value = false;

  std::function<std::expected<std::monostate, std::string>(cli_config &cfg,
                                                           std::string_view)>
      apply = nullptr;
};

static const std::unordered_map<std::string_view, cli_converter>
    k_flag_converters = {
        {"-h",
         {.name = "-h",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.show_help = true;
            return std::monostate{};
          }}},
        {"--help",
         {.name = "--help",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.show_help = true;
            return std::monostate{};
          }}},
        {"--metadata-dir",
         {.name = "--metadata-dir",
          .needs_value = true,
          .apply = +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--metadata-dir requires a non-empty path"}};
            }
            cfg.metadata_dir = std::string{value};
            return std::monostate{};
          }}},
        {"--run",
         {.name = "--run",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.run = true;
            return std::monostate{};
          }}},
        {"--run-function",
         {.name = "--run-function",
          .needs_value = true,
          .apply = +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--run-function requires a non-empty name"}};
            }
            cfg.run = true;
            cfg.run_function = std::string{value};
            return std::monostate{};
          }}},
        {"--compile",
         {.name = "--compile",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.build = true;
            return std::monostate{};
          }}},
        {"--compile-function",
         {.name = "--compile-function",
          .needs_value = true,
          .apply = +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--compile-function requires a non-empty name"}};
            }
            cfg.build = true;
            cfg.build_function = std::string{value};
            return std::monostate{};
          }}},
        {"--compile-output",
         {.name = "--compile-output",
          .needs_value = true,
          .apply = +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--compile-output requires a non-empty path"}};
            }
            cfg.build = true;
            cfg.build_output = std::string{value};
            return std::monostate{};
          }}},
        {"-c",
         {.name = "-c",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.build = true;
            return std::monostate{};
          }}},
        {"-o",
         {.name = "-o",
          .needs_value = true,
          .apply = +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"-o requires a non-empty path"}};
            }
            cfg.build = true;
            cfg.build_output = std::string{value};
            return std::monostate{};
          }}},
        {"--parse-only",
         {.name = "--parse-only",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.parse_only = true;
            return std::monostate{};
          }}},
        {"--show-compile-details",
         {.name = "--show-compile-details",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.show_compile_details = true;
            return std::monostate{};
          }}},
        {"-O",
         {.name = "-O",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            // Bare `-O`, matching gcc/clang's own convention, is shorthand
            // for `-O1`.
            cfg.opt_level = optimization_level::o1;
            return std::monostate{};
          }}},
        {"-O0",
         {.name = "-O0",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.opt_level = optimization_level::o0;
            return std::monostate{};
          }}},
        {"-O1",
         {.name = "-O1",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.opt_level = optimization_level::o1;
            return std::monostate{};
          }}},
        {"-O2",
         {.name = "-O2",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.opt_level = optimization_level::o2;
            return std::monostate{};
          }}},
        {"-O3",
         {.name = "-O3",
          .needs_value = false,
          .apply = +[](cli_config &cfg, std::string_view)
              -> std::expected<std::monostate, std::string> {
            cfg.opt_level = optimization_level::o3;
            return std::monostate{};
          }}},
};

/// Find the first lowered module that defines a function with the given name.
///
/// @param modules Vector of lowered HIR modules to search.
/// @param function_name Name of the function to find.
/// @return Pointer to the module if found, or nullptr.
[[nodiscard]] auto
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

  if (!cfg.build && !cfg.run) {
    cfg.run = true;
  }

  return cfg;
}

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
      "  --compile, -c            Compile `{}` to native code via LLVM, link\n"
      "                          a standalone executable instead of running\n"
      "                          it (same scalar/control-flow subset; see\n"
      "                          src/llvm_codegen)\n"
      "  --compile-function NAME  Like --compile, but use NAME as the entry\n"
      "                          point\n"
      "  --compile-output, -o PATH\n"
      "                          Write the linked executable to PATH\n"
      "  -O0, -O1, -O2, -O3       Optimization level for --compile's LLVM\n"
      "                          output (-O bare means -O1); default -O0,\n"
      "                          no optimization passes run. Has no effect\n"
      "                          on the default bytecode-VM `run` mode\n"
      "  --parse-only             Run only the lexer and parser,\n"
      "                          skipping semantic name resolution and\n"
      "                          type checking\n"
      "  --show-compile-details   Print the per-module compile/HIR-lowering\n"
      "                          listing (silent by default, especially in\n"
      "                          the default interpreting mode, where\n"
      "                          nothing is printed unless there's an\n"
      "                          error)",
      program_name, k_default_metadata_dir, k_default_run_function,
      k_default_run_function, k_default_run_function);
}

} // namespace kira::driver
