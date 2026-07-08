#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kira {

/// Default output directory for serialized module metadata artifacts.
inline constexpr std::string_view k_default_metadata_dir =
    "kira-out/module-metadata";

/// Default entry-point function name executed by `--run`.
inline constexpr std::string_view k_default_run_function = "main";

/// Parsed command-line inputs for one `kira` invocation.
struct cli_config {
  std::string program_name; ///< Executable name shown in usage and diagnostics.
  std::vector<std::string>
      sources; ///< Source file paths to compile in this session.
  std::string metadata_dir =
      std::string(k_default_metadata_dir); ///< Output root for metadata files.
  bool show_help = false; ///< True when argument parsing requested help output.
  bool parse_only = false; ///< Skip name resolution and type checking
                           ///< (parser-focused drivers).
  bool run = false;        ///< Compile to bytecode and execute `run_function`
                           ///< via the tier-0 VM (`src/bytecode/vm.h`) after a
                           ///< successful compile.
  std::string run_function =
      std::string(k_default_run_function); ///< Zero-argument function to
                                        ///< execute when `run` is set.
  bool build = false;                   ///< Compile to a native object file via
                      ///< `src/llvm_codegen`, link it against Kira's AOT
                      ///< runtime support library, and produce a standalone
                      ///< executable (`spec/codegen-design.md` increment 4).
  std::string build_function =
      std::string(k_default_run_function); ///< Zero-argument function to use as
                                        ///< the executable's entry point
                                        ///< when `build` is set.
  std::string build_output; ///< Output executable path; empty derives one
                            ///< from the first source file's stem.
};

/// Metadata artifact written for one successfully compiled module file.
struct compiled_module {
  std::string
      source_path; ///< Normalized source file path that produced the artifact.
  std::vector<std::string>
      module_path; ///< Canonical module path declared by the source file.
  std::string
      metadata_path; ///< Serialized metadata file emitted for the module.
};

/// Best-effort HIR lowering outcome for one module file (see
/// `src/hir/lower.h`). Lowering coverage is still partial — generics,
/// `for`/`while let`/comprehensions over a user-defined iterable, and the
/// concurrency/compile-time forms all still reject — so this is
/// informational only: it does not affect `compile_report::error_count`
/// or whether metadata gets written.
/// A module that fails to lower is not a compilation failure yet.
struct hir_lowering_result {
  std::string
      module_path; ///< Same module label as the matching `compiled_module`.
  bool lowered =
      false;         ///< Whether `hir::lower_module` succeeded for this module.
  std::string error; ///< Lowering error message; empty when `lowered` is true.
};

/// Outcome of executing `cli_config::run_function` when `cli_config::run` is
/// set. Bytecode compilation only covers `spec/codegen-design.md` increment
/// 1's narrow scalar/control-flow subset (see
/// `src/bytecode_compiler/compile.h`), so this is a distinct, best-effort
/// step layered on top of `compile_report::hir_modules` rather than folded
/// into `error_count` — a program that compiles and lowers cleanly may still
/// be unrunnable yet (heap types, generics, ...).
struct run_outcome {
  bool succeeded = false; ///< True when the function was found, compiled, and
                          ///< ran to completion without panicking.
  std::string message;    ///< Rendered return value on success; the reason
                          ///< execution didn't happen or panicked otherwise.
};

/// Outcome of building `cli_config::build_function` into a standalone
/// executable when `cli_config::build` is set. Like `run_outcome`, this is a
/// distinct, best-effort step layered on top of HIR lowering — the same
/// scalar/control-flow subset restriction applies, now via
/// `src/llvm_codegen` rather than `src/bytecode_compiler`.
struct build_outcome {
  bool succeeded = false; ///< True when a standalone executable was produced.
  std::string message;    ///< The output executable's path on success; the
                          ///< reason the build didn't happen otherwise.
};

/// Aggregate result of compiling all requested source files.
struct compile_report {
  std::vector<compiled_module>
      modules;             ///< Metadata artifacts emitted during the session.
  std::string diagnostics; ///< Rendered diagnostics and driver I/O errors.
  uint32_t error_count =
      0; ///< Total error count across parsing and driver validation.
  std::vector<hir_lowering_result>
      hir_modules; ///< Per-module HIR lowering outcomes.
  std::optional<run_outcome>
      run; ///< Populated only when `cli_config::run` was set.
  std::optional<build_outcome>
      build; ///< Populated only when `cli_config::build` was set.
};

/// Parse command-line arguments into driver configuration.
///
/// @param argv Argument vector passed to `main`, including the program name at
/// index 0.
[[nodiscard]] auto parse_args(std::span<char *const> argv)
    -> std::expected<cli_config, std::string>;

/// Render the user-facing CLI help text.
///
/// @param program_name Executable name to display in the usage line.
[[nodiscard]] auto render_help(std::string_view program_name) -> std::string;

/// Parse, validate, and emit metadata for each requested source file.
///
/// @param cfg Command-line configuration that names source inputs and outputs.
/// @param use_color Whether rendered diagnostics should include ANSI colors.
[[nodiscard]] auto compile_sources(const cli_config &cfg,
                                   bool use_color = false)
    -> std::expected<compile_report, std::string>;

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
[[nodiscard]] auto render_compile_summary(const compile_report &report)
    -> std::string;

} // namespace kira
