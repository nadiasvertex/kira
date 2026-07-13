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
  bool show_compile_details =
      false; ///< Print the per-module "Compiled N module(s)"/"Lowered N/M
             ///< module(s) to HIR." listing (and, in `run` mode, the
             ///< executed function's rendered return value) that
             ///< `render_compile_summary` otherwise omits as noise —
             ///< especially unwanted when just interpreting a program.
             ///< Requested via `--show-compile-details`.
  bool parse_only = false; ///< Skip name resolution and type checking
                           ///< (parser-focused drivers).
  bool contract_checks =
      true; ///< Whether a `pre`/`post`/`invariant` the checker could not
            ///< prove becomes a runtime check that panics when violated.
            ///< `--no-contract-checks` is the spec's release elision
            ///< ("Release builds may elide runtime contract checks with an
            ///< explicit flag — doing so is the programmer's assertion that
            ///< all contracts hold by other means").
  bool run = false; ///< Compile to bytecode and execute `run_function`
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
  optimization_level opt_level =
      k_default_optimization_level; ///< `-O0`/`-O1`/`-O2`/`-O3` — only takes
                                    ///< effect on `build`'s LLVM output;
                                    ///< `run` (the bytecode VM) never runs
                                    ///< any LLVM optimization pass.
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
/// @param show_compile_details Include the per-module compile/HIR-lowering
/// listing and (on a successful `run`) the executed function's rendered
/// return value. When false, only failure information is rendered — a
/// clean `run` with nothing to report yields an empty string, matching
/// `cli_config::show_compile_details`'s default of not printing anything
/// when interpreting a program that ran without error.
[[nodiscard]] auto render_compile_summary(const compile_report &report,
                                          bool show_compile_details = false)
    -> std::string;

} // namespace kira::driver
