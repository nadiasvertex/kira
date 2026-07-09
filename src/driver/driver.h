#pragma once

#include <expected>
#include <optional>
#include <string>

#include "aot.h"
#include "defaults.h"
#include "interpret.h"
#include "lower.h"
#include "module_metadata.h"

namespace kira::driver {
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
                           ///< successful compile. `parse_args` defaults this
                           ///< to true whenever `--compile` was not
                           ///< requested, so plain `kira SOURCE` runs it
                           ///< without needing an explicit `--run`.
  std::string run_function =
      std::string(k_default_run_function); ///< Zero-argument function to
                                           ///< execute when `run` is set.
  bool build = false; ///< Compile to a native object file via
                      ///< `src/llvm_codegen`, link it against Kira's AOT
                      ///< runtime support library, and produce a standalone
                      ///< executable (`spec/codegen-design.md` increment 4).
                      ///< Requested via `--compile`.
  std::string build_function =
      std::string(k_default_run_function); ///< Zero-argument function to use as
                                           ///< the executable's entry point
                                           ///< when `build` is set.
  std::string build_output; ///< Output executable path; empty derives one
                            ///< from the first source file's stem.
};

/// Aggregate result of compiling all requested source files.
struct compile_report {
  std::vector<driver::compiled_module>
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

/// Parse, validate, and emit metadata for each requested source file.
///
/// @param cfg Command-line configuration that names source inputs and outputs.
/// @param use_color Whether rendered diagnostics should include ANSI colors.
[[nodiscard]] auto compile_sources(const cli_config &cfg,
                                   bool use_color = false)
    -> std::expected<compile_report, std::string>;

/// Appends `prelude.kira` and `std/traits.kira` (found next to the running
/// binary via the same bundled-data search `find_bazel_archive` uses) to
/// `cfg.sources`, unless a source with the same resolved path is already
/// present. This is how every real `kira` invocation gets the auto-imported
/// prelude — `compile_sources` itself takes exactly the sources it's given,
/// so its own unit tests are unaffected; only `main.cpp`'s real entry point
/// calls this before compiling. Silently does nothing if the stdlib files
/// can't be located (e.g. running a binary copied out of its Bazel tree).
auto inject_stdlib_prelude(cli_config &cfg) -> void;

/// Render a short CLI summary of emitted metadata artifacts and errors.
///
/// @param report Aggregate result of `compile_sources`.
[[nodiscard]] auto render_compile_summary(const compile_report &report)
    -> std::string;

} // namespace kira::driver
