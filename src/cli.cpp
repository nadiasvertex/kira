#include <algorithm>
#include <bit>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "driver/driver.h"

#include <filesystem>

namespace fs = std::filesystem;

#include "driver/interpret.h"
#include "driver/aot.h"
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

namespace kira {

/// Render user-facing visibility text for diagnostics.
///
/// @param visibility Visibility modifier attached to a declaration.
[[nodiscard]] auto visibility_name(ast::visibility visibility)
    -> std::string_view {
  switch (visibility) {
  case ast::visibility::def:
  case ast::visibility::internal:
    return "internal";
  case ast::visibility::pub:
    return "pub";
  case ast::visibility::super:
    return "super";
  case ast::visibility::priv:
    return "priv";
  }
  return "__unspecified__";
}

/// Explain how to make an import valid after a visibility failure.
///
/// @param visibility Visibility modifier attached to the imported declaration.
/// @param parent_name Parent module that owns the imported child module.
[[nodiscard]] auto visibility_help(ast::visibility visibility,
                                   std::string_view parent_name)
    -> std::string {
  switch (visibility) {
  case ast::visibility::pub:
    return std::format(
        "Mark the module `pub` only when it should be importable outside `{}`.",
        parent_name);
  case ast::visibility::def:
  case ast::visibility::internal:
    return std::format("Import this module from `{}` or one of its submodules, "
                       "or widen the declaration's visibility.",
                       parent_name);
  case ast::visibility::super:
    return std::format("Only the parent module `{}` can import this module; "
                       "widen the declaration if other modules need it.",
                       parent_name);
  case ast::visibility::priv:
    return "Keep the import in the declaring file, or widen the module "
           "declaration's visibility.";
  }
  return {};
}

/// Extract the effective top-level visibility from a parsed item node.
///
/// @param node Top-level AST item.
[[nodiscard]] auto top_level_visibility(const ast::node &node)
    -> ast::visibility {
  switch (node.kind) {
  case ast::node_kind::use_decl:
    return dynamic_cast<const ast::use_decl &>(node).visibility;
  case ast::node_kind::type_decl:
    return dynamic_cast<const ast::type_decl &>(node).visibility;
  case ast::node_kind::trait_decl:
    return dynamic_cast<const ast::trait_decl &>(node).visibility;
  case ast::node_kind::concept_decl:
    return dynamic_cast<const ast::concept_decl &>(node).visibility;
  case ast::node_kind::func_decl:
    return dynamic_cast<const ast::func_decl &>(node).visibility;
  case ast::node_kind::sub_module_decl:
    return dynamic_cast<const ast::sub_module_decl &>(node).visibility;
  case ast::node_kind::static_decl:
    return dynamic_cast<const ast::static_decl &>(node).visibility;
  default:
    return ast::visibility::def;
  }
}

/// Extract the user-facing symbol name for a parsed top-level item.
///
/// @param node Top-level AST item.
[[nodiscard]] auto top_level_name(const ast::node &node) -> std::string {
  switch (node.kind) {
  case ast::node_kind::use_decl:
    return join_strings(dynamic_cast<const ast::use_decl &>(node).path, ".");
  case ast::node_kind::type_decl:
    return dynamic_cast<const ast::type_decl &>(node).name;
  case ast::node_kind::trait_decl:
    return dynamic_cast<const ast::trait_decl &>(node).name;
  case ast::node_kind::concept_decl:
    return dynamic_cast<const ast::concept_decl &>(node).name;
  case ast::node_kind::func_decl:
    return dynamic_cast<const ast::func_decl &>(node).name;
  case ast::node_kind::sub_module_decl:
    return dynamic_cast<const ast::sub_module_decl &>(node).name;
  case ast::node_kind::dep_decl:
    return dynamic_cast<const ast::dep_decl &>(node).name;
  case ast::node_kind::static_decl:
    return static_decl_label(dynamic_cast<const ast::static_decl &>(node));
  case ast::node_kind::error_node:
    return dynamic_cast<const ast::error_node &>(node).description;
  default:
    return {};
  }
}

/// Remove surrounding quotes from dependency string literals before persisting
/// them.
///
/// @param value Parsed dependency field value.
[[nodiscard]] auto unquote_string_literal(std::string_view value)
    -> std::string {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    return std::string(value.substr(1, value.size() - 2));
  }
  return std::string(value);
}


/// Serialize one module's metadata file and return its output path.
///
/// @param metadata_root Root directory configured for metadata output.
/// @param file Parsed AST for the source file.
/// @param source_path Original source file path.
/// @param diagnostics Plain-text driver diagnostics buffer for I/O failures.
[[nodiscard]] auto
write_module_metadata(const fs::path &metadata_root, const ast::file &file,
                      const fs::path &source_path, std::string &diagnostics)
    -> std::expected<std::string, std::monostate> {
  auto output_path = metadata_output_path(metadata_root, file, source_path);
  auto output_parent = output_path.parent_path();

  auto ec = std::error_code{};
  if (!output_parent.empty()) {
    fs::create_directories(output_parent, ec);
    if (ec) {
      append_error(diagnostics,
                   std::format("failed to create `{}`: {}",
                               normalize_path(output_parent), ec.message()));
      return std::unexpected(std::monostate{});
    }
  }

  auto out = std::ofstream(output_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    append_error(diagnostics, std::format("failed to open `{}` for writing",
                                          normalize_path(output_path)));
    return std::unexpected(std::monostate{});
  }

  auto metadata = build_module_metadata(file, source_path);
  if (!metadata.SerializeToOstream(&out) || !out.good()) {
    out.close();
    fs::remove(output_path, ec);
    append_error(diagnostics,
                 std::format("failed to serialize module metadata to `{}`",
                             normalize_path(output_path)));
    return std::unexpected(std::monostate{});
  }

  return normalize_path(output_path);
}

/// Parsed source file plus the bookkeeping needed by later driver passes.
struct parsed_input {
  fs::path source_path;     ///< Original source file path passed to the CLI.
  file_id_type file_id = 0; ///< Source manager file identifier for diagnostics.
  ast::ptr<ast::file> ast_file; ///< Parsed AST for the source file.
};

/// Renders a VM return value for `--run`'s output, using the function's
/// checked return type to pick how `slot_value`'s untagged union should be
/// read back (mirrors `bytecode_compiler::encode_literal`'s reverse
/// direction).
///
/// @param types Checked type table the function's `return_type` indexes
/// into.
/// @param return_type Checked return type of the executed function.
/// @param value Raw VM result to render.
[[nodiscard]] auto render_run_value(const semantic::type_table &types,
                                    semantic::type_id return_type,
                                    const bytecode::slot_value &value)
    -> std::string {
  const auto kind = bytecode::numeric_kind_of(types, return_type);
  if (!kind) {
    return "<unsupported return type>";
  }

  switch (*kind) {
  case bytecode::numeric_kind::i8:
  case bytecode::numeric_kind::i16:
  case bytecode::numeric_kind::i32:
  case bytecode::numeric_kind::i64:
    return std::format("{}", value.i);
  case bytecode::numeric_kind::u8:
  case bytecode::numeric_kind::u16:
  case bytecode::numeric_kind::u32:
  case bytecode::numeric_kind::u64:
    return std::format("{}", value.u);
  case bytecode::numeric_kind::f32:
    return std::format("{}",
                       std::bit_cast<float>(static_cast<uint32_t>(value.u)));
  case bytecode::numeric_kind::f64:
    return std::format("{}", value.f);
  case bytecode::numeric_kind::boolean:
    return value.i != 0 ? "true" : "false";
  case bytecode::numeric_kind::character:
    return std::format("U+{:04X}", value.u);
  }
  return {};
}

/// Compiles `module` to bytecode and executes `function_name` with no
/// arguments, producing the `--run` outcome shown in the CLI summary.
/// `--run` targets exactly `spec/codegen-design.md` increment 1's subset —
/// this project has no CLI-level argument-marshalling story yet, so only
/// zero-parameter functions are runnable — and fails closed (a message, not
/// a crash) on every other reason execution can't proceed.
///
/// @param hir_module Lowered module to compile and run.
/// @param types Checked type table the module's HIR indexes into.
/// @param function_name Name of the zero-argument function to execute.
[[nodiscard]] auto run_hir_module(const hir::hir_module &hir_module,
                                  const semantic::type_table &types,
                                  std::string_view function_name)
    -> run_outcome {
  const auto *target = static_cast<const hir::hir_function *>(nullptr);
  for (const auto &fn : hir_module.functions) {
    if (fn != nullptr && fn->name == function_name) {
      target = fn.get();
      break;
    }
  }

  if (target == nullptr) {
    return run_outcome{.succeeded = false,
                       .message =
                           std::format("module `{}` has no function named `{}`",
                                       hir_module.module_name, function_name)};
  }

  if (!target->params.empty()) {
    return run_outcome{
        .succeeded = false,
        .message = std::format(
            "cannot run `{}`: --run only supports zero-parameter functions",
            function_name)};
  }

  auto compiled = bytecode_compiler::compile_module(hir_module, types);
  if (!compiled) {
    return run_outcome{.succeeded = false,
                       .message = std::format("failed to compile `{}` to "
                                              "bytecode: {}",
                                              function_name,
                                              compiled.error().message)};
  }

  auto index = uint16_t{0};
  const auto function_count = static_cast<uint16_t>(compiled->functions.size());
  for (; index < function_count; ++index) {
    if (compiled->functions[index].name == function_name) {
      break;
    }
  }

  const auto vm = bytecode::vm{*compiled};
  auto result = vm.run(index, std::span<const bytecode::slot_value>{});
  if (!result) {
    return run_outcome{
        .succeeded = false,
        .message = std::format("`{}` panicked: {}", function_name,
                               bytecode::panic_reason_message(result.error()))};
  }

  if (!result->has_value) {
    return run_outcome{.succeeded = true,
                       .message = std::format("{}() -> ()", function_name)};
  }

  return run_outcome{
      .succeeded = true,
      .message = std::format(
          "{}() -> {}", function_name,
          render_run_value(types, target->return_type, result->value))};
}

/// Locates one `alwayslink = True` cc_library's archive under `bazel_package`
/// (e.g. `src/llvm_codegen`) named `library_name` (e.g. `aot_runtime`), so
/// `--compile` can hand it to the system linker. There is no installed-location
/// story yet (`just package` doesn't bundle these) — this only finds an
/// archive when `kira` itself is run from within the Bazel workspace that
/// built it (via `bazelisk run //src:kira`, directly from `bazel-bin`, or —
/// so this same lookup is testable under `bazel test` — from a `cc_test`
/// that declares the archive as a `data` dependency): mirrors how
/// `driver_stress_test.cpp`/`semantic_stress_test.cpp` locate their own test
/// corpora through Bazel runfiles (`src/testing/test_data.h`'s
/// `candidate_test_data_dirs`, same `TEST_SRCDIR`/`TEST_WORKSPACE`
/// convention).
///
/// @param program_name `argv[0]` this process was invoked with.
[[nodiscard]] auto find_bazel_archive(std::string_view program_name,
                                      std::string_view bazel_package,
                                      std::string_view library_name)
    -> std::optional<fs::path> {
  auto candidates = std::vector<fs::path>{};
  // Bazel names an `alwayslink = True` cc_library's archive `.lo`, not
  // `.a` (still an ordinary `ar` archive under the hood) — check both
  // since that's an implementation detail of the Bazel version in use, not
  // something this driver should hardcode a single answer for.
  for (const auto *extension : {"lo", "a"}) {
    const auto filename = std::format("lib{}.{}", library_name, extension);
    if (!program_name.empty()) {
      candidates.emplace_back(
          fs::path(std::format("{}.runfiles", program_name)) / "_main" /
          bazel_package / filename);
    }
    if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
      if (const auto *workspace = std::getenv("TEST_WORKSPACE");
          workspace != nullptr && *workspace != '\0') {
        candidates.emplace_back(fs::path(srcdir) / workspace / bazel_package /
                                filename);
      }
      candidates.emplace_back(fs::path(srcdir) / "_main" / bazel_package /
                              filename);
    }
    candidates.emplace_back(fs::path("bazel-bin") / bazel_package / filename);
  }

  for (const auto &candidate : candidates) {
    auto ec = std::error_code{};
    if (fs::exists(candidate, ec)) {
      return candidate;
    }
  }
  return std::nullopt;
}

/// Compiles `hir_module` to a native object file via `src/llvm_codegen`,
/// then links it against Kira's AOT runtime support library into a
/// standalone executable at `output_path` — `--compile`'s counterpart to
/// `run_hir_module`, using the same "zero-argument entry function" scope
/// limit (`llvm_codegen::emit_object_file`'s own doc comment) and the same
/// fail-closed-with-a-message discipline.
///
/// @param hir_module Lowered module to compile and link.
/// @param types Checked type table the module's HIR indexes into.
/// @param function_name Name of the zero-argument function to use as the
/// executable's entry point.
/// @param output_path Destination path for the linked executable.
/// @param program_name `argv[0]`, used to locate the AOT runtime archive.
[[nodiscard]] auto build_hir_module(const hir::hir_module &hir_module,
                                    const semantic::type_table &types,
                                    std::string_view function_name,
                                    const fs::path &output_path,
                                    std::string_view program_name)
    -> build_outcome {
  auto compiled = llvm_codegen::compile_module(hir_module, types);
  if (!compiled) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("failed to compile `{}` to native code: {}",
                               function_name, compiled.error().message)};
  }

  const auto object_path =
      fs::temp_directory_path() /
      std::format("kira-build-{}.o", static_cast<long>(::getpid()));
  auto emitted = llvm_codegen::emit_object_file(
      std::move(*compiled), function_name, object_path.string());
  if (!emitted) {
    return build_outcome{.succeeded = false,
                         .message = std::format("failed to emit an object "
                                                "file for `{}`: {}",
                                                function_name,
                                                emitted.error().message)};
  }

  const auto panic_archive =
      find_bazel_archive(program_name, "src/llvm_codegen", "aot_runtime");
  if (!panic_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's AOT panic runtime support "
                   "library (libaot_runtime.a) — run `kira` via `bazelisk "
                   "run //src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }
  // Statically links the same bump-arena heap allocator (`kira_rt_alloc`,
  // src/runtime/arena.h) `llvm_codegen`-compiled IR calls for every
  // non-scalar value (spec/codegen-design.md Decision 3) — an AOT binary
  // has no libLLVM/bytecode-VM dependency (Decision 5), but it does still
  // need this one small runtime support library, the same as the panic
  // archive above.
  const auto heap_archive =
      find_bazel_archive(program_name, "src/runtime", "runtime");
  if (!heap_archive) {
    return build_outcome{
        .succeeded = false,
        .message = "could not locate Kira's heap runtime support library "
                   "(libruntime.a) — run `kira` via `bazelisk run "
                   "//src:kira` or from `bazel-bin/src/kira` inside the "
                   "workspace that built it"};
  }

  // `c++` (not `cc`) deliberately — `arena.cpp`/`aot_runtime.cpp`/`io.cpp`
  // all use the C++ standard library internally (std::vector, std::format,
  // exceptions), so any program whose object file actually references a
  // symbol from either archive (any heap type, any checked-arithmetic
  // panic path, any intrinsic call) needs libc++/libc++abi linked in.
  // Plain `cc` doesn't do that automatically; `c++`/`clang++` does.
  const auto link_command = std::format(
      R"(c++ "{}" "{}" "{}" -o "{}")", object_path.string(),
      panic_archive->string(), heap_archive->string(), output_path.string());
  const auto link_status = std::system(link_command.c_str());
  auto ec = std::error_code{};
  fs::remove(object_path, ec);
  if (link_status != 0) {
    return build_outcome{
        .succeeded = false,
        .message = std::format("linking failed (exit status {})", link_status)};
  }

  return build_outcome{.succeeded = true, .message = output_path.string()};
}

/// Handler for a parsed CLI option.
struct cli_converter {
  /// Flag name that triggers this converter (e.g. `"--metadata-dir"`).
  std::string_view name;

  /// Whether the flag expects a separate value argument.
  bool needs_value = false;

  /// Action to take when the flag is matched and its value resolved or
  /// supplied inline.  Returns an error if the action should abort parsing.
  std::function<std::expected<std::monostate, std::string>(cli_config &cfg,
                                                           std::string_view)>
      apply = nullptr;
};

/// Registry of all accepted flags.
static const std::unordered_map<std::string_view, cli_converter>
    k_flag_converters = {
        {"-h",
         {"-h", false,
          +[](cli_config &cfg,
              std::string_view) -> std::expected<std::monostate, std::string> {
            cfg.show_help = true;
            return std::monostate{};
          }}},
        {"--help",
         {"--help", false,
          +[](cli_config &cfg,
              std::string_view) -> std::expected<std::monostate, std::string> {
            cfg.show_help = true;
            return std::monostate{};
          }}},
        {"--metadata-dir",
         {"--metadata-dir", true,
          +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--metadata-dir requires a non-empty path"}};
            }
            cfg.metadata_dir = std::string{value};
            return std::monostate{};
          }}},
        {"--run",
         {"--run", false,
          +[](cli_config &cfg,
              std::string_view) -> std::expected<std::monostate, std::string> {
            cfg.run = true;
            return std::monostate{};
          }}},
        {"--run-function",
         {"--run-function", true,
          +[](cli_config &cfg, std::string_view value)
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
         {"--compile", false,
          +[](cli_config &cfg,
              std::string_view) -> std::expected<std::monostate, std::string> {
            cfg.build = true;
            return std::monostate{};
          }}},
        {"--compile-function",
         {"--compile-function", true,
          +[](cli_config &cfg, std::string_view value)
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
         {"--compile-output", true,
          +[](cli_config &cfg, std::string_view value)
              -> std::expected<std::monostate, std::string> {
            if (value.empty()) {
              return std::unexpected{
                  std::string{"--compile-output requires a non-empty path"}};
            }
            cfg.build = true;
            cfg.build_output = std::string{value};
            return std::monostate{};
          }}},
        {"--parse-only",
         {"--parse-only", false,
          +[](cli_config &cfg,
              std::string_view) -> std::expected<std::monostate, std::string> {
            cfg.parse_only = true;
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

} // namespace

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
