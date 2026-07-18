#include "driver/cli.h"

#include <sys/wait.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "src/module_metadata.pb.h"

namespace {

namespace fs = std::filesystem;

/// Abort the test binary immediately when an assertion fails.
///
/// @param condition Condition that must hold.
/// @param message Failure message printed before exiting.
auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    std::cerr << "cli_test failed: " << message << '\n';
    std::exit(1);
  }
}

/// Build an argv-style vector of mutable C strings from string arguments.
auto make_argv(std::vector<std::string> &args) -> std::vector<char *> {
  std::vector<char *> argv;
  argv.reserve(args.size());
  for (auto &arg : args) {
    argv.push_back(arg.data());
  }
  return argv;
}

/// Verify that plain source paths are accepted as positional arguments.
auto test_parse_args_accepts_sources() -> void {
  std::vector<std::string> args = {"kira", "main.kira", "lib.kira"};
  auto argv = make_argv(args);

  auto result = kira::driver::parse_args(argv);
  expect(result.has_value(), "expected sources to parse successfully");
  expect(!result->show_help, "plain sources should not request help");
  expect(result->sources.size() == 2, "expected two source arguments");
  expect(result->sources[0] == "main.kira", "expected first source path");
  expect(result->sources[1] == "lib.kira", "expected second source path");
  expect(result->metadata_dir == kira::driver::k_default_metadata_dir,
         "expected default metadata directory");
}

/// Verify help handling and `--` option termination.
auto test_parse_args_supports_help_and_double_dash() -> void {
  std::vector<std::string> help_args = {"kira", "--help"};
  auto help_argv = make_argv(help_args);

  auto help_result = kira::driver::parse_args(help_argv);
  expect(help_result.has_value(), "help should parse successfully");
  expect(help_result->show_help, "--help should set show_help");

  std::vector<std::string> dash_args = {"kira", "--", "-literal.kira"};
  auto dash_argv = make_argv(dash_args);

  auto dash_result = kira::driver::parse_args(dash_argv);
  expect(dash_result.has_value(), "-- should stop option parsing");
  expect(dash_result->sources.size() == 1, "expected one source after --");
  expect(dash_result->sources[0] == "-literal.kira",
         "expected source after -- to be preserved");
}

/// Verify `--version` sets `show_version` without requiring a source file.
auto test_parse_args_supports_version() -> void {
  std::vector<std::string> args = {"kira", "--version"};
  auto argv = make_argv(args);

  auto result = kira::driver::parse_args(argv);
  expect(result.has_value(), "--version should parse successfully");
  expect(result->show_version, "--version should set show_version");
  expect(result->sources.empty(), "--version should not require sources");
}

/// Verify that the metadata output directory can be overridden.
auto test_parse_args_accepts_metadata_dir() -> void {
  std::vector<std::string> args = {"kira", "--metadata-dir", "build/meta",
                                   "main.kira"};
  auto argv = make_argv(args);

  auto result = kira::driver::parse_args(argv);
  expect(result.has_value(), "metadata dir option should parse successfully");
  expect(result->metadata_dir == "build/meta",
         "expected metadata directory override");
}

/// Verify that unknown command-line options are rejected.
auto test_parse_args_rejects_unknown_options() -> void {
  std::vector<std::string> args = {"kira", "--bogus"};
  auto argv = make_argv(args);

  auto result = kira::driver::parse_args(argv);
  expect(!result.has_value(), "unknown options should fail");
  expect(result.error() == "unknown option: --bogus",
         "expected unknown option error message");
}

/// Verify that `--show-compile-details` is off by default and settable.
auto test_parse_args_accepts_show_compile_details() -> void {
  std::vector<std::string> default_args = {"kira", "main.kira"};
  auto default_argv = make_argv(default_args);

  auto default_result = kira::driver::parse_args(default_argv);
  expect(default_result.has_value(), "expected sources to parse successfully");
  expect(!default_result->show_compile_details,
         "expected show_compile_details to default to false");

  std::vector<std::string> args = {"kira", "--show-compile-details",
                                   "main.kira"};
  auto argv = make_argv(args);

  auto result = kira::driver::parse_args(argv);
  expect(result.has_value(),
         "--show-compile-details should parse successfully");
  expect(result->show_compile_details,
         "--show-compile-details should set show_compile_details");
}

/// Verify `-O0`/`-O1`/`-O2`/`-O3`/bare `-O` parse into `cli_config::opt_level`,
/// and that the default (no flag at all) stays `o0`.
auto test_parse_args_accepts_optimization_level() -> void {
  std::vector<std::string> default_args = {"kira", "main.kira"};
  auto default_argv = make_argv(default_args);
  auto default_result = kira::driver::parse_args(default_argv);
  expect(default_result.has_value(), "expected sources to parse successfully");
  expect(default_result->opt_level == kira::driver::optimization_level::o0,
         "expected opt_level to default to o0 with no -O flag");

  std::vector<std::string> bare_args = {"kira", "-O", "main.kira"};
  auto bare_argv = make_argv(bare_args);
  auto bare_result = kira::driver::parse_args(bare_argv);
  expect(bare_result.has_value(), "-O should parse successfully");
  expect(bare_result->opt_level == kira::driver::optimization_level::o1,
         "expected bare -O to mean -O1");

  const auto levels =
      std::array<std::pair<std::string, kira::driver::optimization_level>, 4>{{
          {"-O0", kira::driver::optimization_level::o0},
          {"-O1", kira::driver::optimization_level::o1},
          {"-O2", kira::driver::optimization_level::o2},
          {"-O3", kira::driver::optimization_level::o3},
      }};
  for (const auto &[flag, expected] : levels) {
    std::vector<std::string> args = {"kira", flag, "main.kira"};
    auto argv = make_argv(args);
    auto result = kira::driver::parse_args(argv);
    expect(result.has_value(),
           std::format("{} should parse successfully", flag));
    expect(result->opt_level == expected,
           std::format("expected {} to set the matching opt_level", flag));
  }
}

/// Verify help and compile-summary rendering helpers.
auto test_rendering_helpers() -> void {
  auto help = kira::driver::render_help("kira");
  expect(help.find("Usage: kira [OPTIONS] SOURCES...") != std::string::npos,
         "help should contain usage line");
  expect(help.find("Kira - Parse source files and emit module metadata") !=
             std::string::npos,
         "help should contain description");
  expect(help.find("--metadata-dir PATH") != std::string::npos,
         "help should document metadata dir option");

  kira::driver::compile_report report{
      .modules = {{
          .source_path = "main.kira",
          .module_path = {"sample", "tools"},
          .metadata_path = "build/meta/sample/tools.kmeta.pb",
      }},
      .diagnostics = {},
      .error_count = 0,
  };
  auto quiet_summary = kira::driver::render_compile_summary(report);
  expect(quiet_summary.empty(),
         "summary should be silent by default for a clean compile");

  auto summary = kira::driver::render_compile_summary(report, true);
  expect(summary.find("Compiled 1 module(s):") != std::string::npos,
         "summary should report compiled module count with "
         "--show-compile-details");
  expect(summary.find("sample.tools -> build/meta/sample/tools.kmeta.pb") !=
             std::string::npos,
         "summary should include metadata output path");
}

/// Temporary directory that cleans itself up when the test ends.
struct temp_dir {
  fs::path path; ///< Root directory allocated for one test case.

  /// Remove the temporary directory tree created for the test.
  ~temp_dir() {
    auto ec = std::error_code{};
    fs::remove_all(path, ec);
  }
};

/// Create a fresh temporary directory for one test case.
auto make_temp_dir() -> temp_dir {
  auto base =
      fs::temp_directory_path() /
      std::format("kira_cli_test_{}",
                  std::chrono::steady_clock::now().time_since_epoch().count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, "expected to create temporary test directory");
  return temp_dir{.path = std::move(base)};
}

/// Write one test fixture file, creating parent directories as needed.
///
/// @param path Destination file path.
/// @param contents File contents to write.
auto write_file(const fs::path &path, std::string_view contents) -> void {
  auto ec = std::error_code{};
  fs::create_directories(path.parent_path(), ec);
  expect(!ec, "expected to create parent directory for test file");

  auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
  expect(static_cast<bool>(out), "expected to open test file for writing");
  out << contents;
  expect(out.good(), "expected to write test file contents");
}

/// Verify that successful compilation writes parse metadata for one file.
auto test_compile_sources_writes_module_metadata() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_tools.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample.tools\n"
                          "no_prelude\n"
                          "use std.io\n"
                          "dep sqlite:\n"
                          "  version = \"3.45\"\n"
                          "#: Runs the tool.\n"
                          "#: Returns an exit code.\n"
                          "pub def run() -> int32:\n"
                          "  return 1\n"
                          "type person = { name: str }\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->modules.size() == 1, "expected one metadata artifact");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for valid source");

  auto metadata_path = fs::path(report->modules[0].metadata_path);
  expect(fs::exists(metadata_path), "expected metadata file to be written");

  auto in = std::ifstream(metadata_path, std::ios::binary);
  expect(static_cast<bool>(in), "expected metadata file to open");

  auto metadata = kira::metadata::v1::ModuleMetadata{};
  expect(metadata.ParseFromIstream(&in), "expected metadata protobuf to parse");
  expect(metadata.schema_version() == 3, "expected metadata schema version");
  expect(metadata.module_path_size() == 2, "expected module path components");
  expect(metadata.module_path(0) == "sample",
         "expected first module path part");
  expect(metadata.module_path(1) == "tools",
         "expected second module path part");
  expect(metadata.no_prelude(), "expected no_prelude flag to be preserved");
  expect(metadata.imports_size() == 1, "expected one import in metadata");
  expect(metadata.dependencies_size() == 1,
         "expected one dependency declaration in metadata");
  expect(metadata.top_level_symbols_size() == 4,
         "expected top-level declarations to be recorded");
  expect(metadata.top_level_symbols(2).kind() ==
             kira::metadata::v1::TOP_LEVEL_SYMBOL_KIND_FUNCTION,
         "expected function symbol kind");
  expect(metadata.top_level_symbols(2).name() == "run",
         "expected function name in metadata");
  expect(metadata.top_level_symbols(2).visibility() ==
             kira::metadata::v1::MODULE_VISIBILITY_PUBLIC,
         "expected function visibility in metadata");
  expect(metadata.top_level_symbols(2).documentation() ==
             "Runs the tool.\nReturns an exit code.",
         "expected joined docstring to be recorded in metadata");
  expect(metadata.top_level_symbols(3).documentation().empty(),
         "expected undocumented `person` type to have empty documentation");
  expect(metadata.dependencies(0).fields().at("version") == "3.45",
         "expected dependency field value to be unquoted");
}

/// Verify that a functor instantiation (`use m[args] as alias`) is recorded in
/// the emitting file's metadata with its functor path, arguments, alias, and a
/// canonical-ish instantiation key.
auto test_compile_sources_writes_functor_instantiation_metadata() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module app\n"
                          "signature backend:\n"
                          "    type conn\n"
                          "    def connect(url: str) -> conn\n"
                          "module postgres:\n"
                          "    pub type conn = int32\n"
                          "    pub def connect(url: str) -> conn:\n"
                          "        return 0\n"
                          "module audited[DB: backend]:\n"
                          "    pub def go() -> int32:\n"
                          "        return 0\n"
                          "use app.audited[app.postgres] as db\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid functor source to compile");
  expect(report->modules.size() == 1, "expected one metadata artifact");

  auto in = std::ifstream(fs::path(report->modules[0].metadata_path),
                          std::ios::binary);
  auto metadata = kira::metadata::v1::ModuleMetadata{};
  expect(metadata.ParseFromIstream(&in), "expected metadata protobuf to parse");
  expect(metadata.functor_instantiations_size() == 1,
         "expected one functor instantiation in metadata");
  const auto &inst = metadata.functor_instantiations(0);
  expect(inst.functor_path_size() == 2 && inst.functor_path(0) == "app" &&
             inst.functor_path(1) == "audited",
         "expected the functor path `app.audited`");
  expect(inst.argument_size() == 1 && inst.argument(0) == "app.postgres",
         "expected the argument spelling `app.postgres`");
  expect(inst.alias() == "db", "expected the `as db` alias");
  expect(inst.instantiation_key() == "app.audited[app.postgres]",
         "expected the canonical instantiation key");
}

/// Verify that an import-gating `static if` with a literal condition folds to
/// its taken branch before the module graph is built: the taken branch's `use`
/// is active (its API resolves) and the untaken branch's is not.
auto test_compile_sources_folds_static_if_import_selection() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "app.kira";
  auto metadata_dir = temp.path / "meta";

  // `real_io` exposes `alpha`, `fake_io` exposes `beta`. The `static if true`
  // branch imports `real_io as io`, so `io.alpha()` must resolve; had the fold
  // wrongly selected the `else` branch, `io.alpha()` would be undefined.
  write_file(source_path, "module app\n"
                          "module real_io:\n"
                          "    pub def alpha() -> int32:\n"
                          "        return 1\n"
                          "module fake_io:\n"
                          "    pub def beta() -> int32:\n"
                          "        return 2\n"
                          "static if true:\n"
                          "    use app.real_io as io\n"
                          "else:\n"
                          "    use app.fake_io as io\n"
                          "def use_it() -> int32:\n"
                          "    return io.alpha()\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         std::string("expected the `static if true` branch's `use real_io as "
                     "io` to be selected so `io.alpha()` resolves:\n") +
             report->diagnostics);
}

/// Verify that a `use`-gating `static if` whose condition is not early-
/// evaluable (it references a `static let`, which needs name resolution) is
/// rejected with the restriction diagnostic rather than silently dropping the
/// import from the module graph.
auto test_compile_sources_rejects_use_gated_by_nonliteral_static_if() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module app\n"
                          "module real_io:\n"
                          "    pub def alpha() -> int32:\n"
                          "        return 1\n"
                          "static let flag: bool = true\n"
                          "static if flag:\n"
                          "    use app.real_io as io\n"
                          "def run() -> int32:\n"
                          "    return 0\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(
      report->error_count > 0,
      "expected a non-early-evaluable `use`-gating condition to be rejected");
  expect(report->diagnostics.find("early-evaluable") != std::string::npos,
         std::string("expected the restriction diagnostic:\n") +
             report->diagnostics);
}

/// Verify that a fully-annotated module also lowers to HIR alongside its
/// metadata, and that the outcome is recorded on the report.
auto test_compile_sources_lowers_module_to_hir() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_math.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample.math\n"
                          "pub def add(a: int32, b: int32) -> int32:\n"
                          "  return a + b\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->hir_modules.size() == 1, "expected one HIR lowering outcome");
  expect(report->hir_modules[0].lowered,
         "expected a fully-annotated module to lower successfully");
  expect(report->hir_modules[0].module_path == "sample.math",
         "expected the lowering outcome to name the module");
  expect(report->hir_modules[0].error.empty(),
         "expected no error message for a successful lowering");

  auto summary = kira::driver::render_compile_summary(*report, true);
  expect(summary.find("Lowered 1/1 module(s) to HIR.") != std::string::npos,
         "expected the summary to report the lowering outcome");
}

/// Verify that a module using a construct lowering doesn't support yet
/// (here, `for` iteration over a user-defined type) still compiles and
/// writes metadata normally — HIR lowering is best-effort and must never
/// gate ordinary compilation.
auto test_compile_sources_records_hir_lowering_failure_without_failing_compile()
    -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_loop.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample.loop\n"
                          "type counter = { pub value: int32 }\n"
                          "pub def sum_counters(xs: counter) -> int32:\n"
                          "  var total = 0\n"
                          "  for x in xs:\n"
                          "    total = total + x\n"
                          "  return total\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a for-loop module to still compile cleanly");
  expect(report->modules.size() == 1,
         "expected metadata to still be written despite the lowering gap");
  expect(report->hir_modules.size() == 1, "expected one HIR lowering outcome");
  expect(!report->hir_modules[0].lowered,
         "expected the `for` loop to fail lowering");
  expect(!report->hir_modules[0].error.empty(),
         "expected a lowering error message to be recorded");
}

/// Verify that `--parse-only`-equivalent sessions (checking skipped) also
/// skip HIR lowering rather than lowering against an empty checked-types
/// result.
auto test_compile_sources_skips_lowering_when_parse_only() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_parse_only.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample.parse_only\n"
                          "pub def add(a: int32, b: int32) -> int32:\n"
                          "  return a + b\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .parse_only = true,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->hir_modules.empty(),
         "expected no HIR lowering outcomes when parse_only skips checking");
}

/// Verify that parser failures are reported and block metadata output.
auto test_compile_sources_reports_parser_errors() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "broken.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "def broken():\n"
                          "  return 1\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected compile driver to report parser failures");
  expect(report->error_count > 0, "expected parser errors for invalid source");
  expect(report->modules.empty(), "expected no metadata artifacts on failure");
  expect(report->diagnostics.find(
             "every Kira source file must start with a `module` declaration") !=
             std::string::npos,
         "expected missing-module diagnostic");
  expect(!fs::exists(metadata_dir),
         "expected no metadata directory on failure");
}

/// Verify that malformed nested blocks still terminate instead of looping.
auto test_compile_sources_reports_nested_parser_errors() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "nested_broken.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "module util:\n"
                          "  pub def shared() -> int32:\n"
                          "    return 1\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected nested parser failure to return a report");
  expect(report->error_count > 0,
         "expected nested parser failure to report errors");
  expect(report->modules.empty(),
         "expected nested parser failure to block metadata output");
  expect(!fs::exists(metadata_dir),
         "expected nested parser failure to skip metadata output");
}

/// Verify that multiple valid source files compile in one session.
auto test_compile_sources_handles_multiple_files() -> void {
  auto temp = make_temp_dir();
  auto source_a = temp.path / "sample_tools.kira";
  auto source_b = temp.path / "sample_math.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_a, "module sample.tools\n"
                       "pub def run() -> int32:\n"
                       "  return 1\n");
  write_file(source_b, "module sample.math\n"
                       "pub def add() -> int32:\n"
                       "  return 2\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_a.string(), source_b.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected multi-file compile to return a report");
  expect(report->error_count == 0, "expected multi-file compile to succeed");
  expect(report->modules.size() == 2, "expected two metadata artifacts");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for valid inputs");
  expect(fs::exists(fs::path(report->modules[0].metadata_path)),
         "expected first metadata file to exist");
  expect(fs::exists(fs::path(report->modules[1].metadata_path)),
         "expected second metadata file to exist");
}

/// Verify that duplicate module paths are rejected across files.
auto test_compile_sources_reports_duplicate_module_paths() -> void {
  auto temp = make_temp_dir();
  auto source_a = temp.path / "first.kira";
  auto source_b = temp.path / "second.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_a, "module sample.tools\n"
                       "pub def first() -> int32:\n"
                       "  return 1\n");
  write_file(source_b, "module sample.tools\n"
                       "pub def second() -> int32:\n"
                       "  return 2\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_a.string(), source_b.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected duplicate-module compile to return a report");
  expect(report->error_count > 0, "expected duplicate module path to fail");
  expect(report->modules.empty(),
         "expected duplicate modules to block metadata output");
  expect(report->diagnostics.find("duplicate module path `sample.tools`") !=
             std::string::npos,
         "expected duplicate-module diagnostic");
  expect(report->diagnostics.find("first.kira") != std::string::npos,
         "expected diagnostic to mention first file");
  expect(report->diagnostics.find("second.kira") != std::string::npos,
         "expected diagnostic to mention second file");
  expect(!fs::exists(metadata_dir),
         "expected duplicate modules to skip metadata output entirely");
}

/// Verify that declared child modules may be compiled in separate files.
auto test_compile_sources_accepts_declared_external_submodule() -> void {
  auto temp = make_temp_dir();
  auto parent_source = temp.path / "geometry.kira";
  auto child_source = temp.path / "geometry_transform.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(parent_source, "module geometry\n"
                            "module transform\n"
                            "pub def root() -> int32:\n"
                            "  return 1\n");
  write_file(child_source, "module geometry.transform\n"
                           "pub def rotate() -> int32:\n"
                           "  return 2\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected declared submodule compile to return a report");
  expect(report->error_count == 0,
         "expected declared external submodule to compile cleanly");
  expect(report->modules.size() == 2,
         "expected parent and child metadata artifacts");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for declared external submodule");
}

/// Verify that child modules need a matching declaration in their parent
/// module.
auto test_compile_sources_reports_missing_parent_submodule_declaration()
    -> void {
  auto temp = make_temp_dir();
  auto parent_source = temp.path / "geometry.kira";
  auto child_source = temp.path / "geometry_transform.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(parent_source, "module geometry\n"
                            "pub def root() -> int32:\n"
                            "  return 1\n");
  write_file(child_source, "module geometry.transform\n"
                           "pub def rotate() -> int32:\n"
                           "  return 2\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected missing-parent compile to return a report");
  expect(report->error_count > 0,
         "expected undeclared external submodule to fail");
  expect(report->modules.empty(),
         "expected undeclared external submodule to block metadata output");
  expect(report->diagnostics.find("module `geometry.transform` is not declared "
                                  "by parent module `geometry`") !=
             std::string::npos,
         "expected missing parent declaration diagnostic");
  expect(report->diagnostics.find("module transform") != std::string::npos,
         "expected help text to mention missing submodule declaration");
}

/// Verify that inline and separate-file definitions of the same child module
/// conflict.
auto test_compile_sources_reports_inline_external_submodule_conflict() -> void {
  auto temp = make_temp_dir();
  auto parent_source = temp.path / "geometry.kira";
  auto child_source = temp.path / "geometry_shapes.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(parent_source, "module geometry\n"
                            "module shapes:\n"
                            "  pub type circle = { pub radius: float64 }\n");
  write_file(child_source, "module geometry.shapes\n"
                           "pub def area() -> int32:\n"
                           "  return 3\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected inline-conflict compile to return a report");
  expect(report->error_count > 0,
         "expected inline and external submodule conflict to fail");
  expect(report->modules.empty(), "expected inline and external submodule "
                                  "conflict to block metadata output");
  expect(report->diagnostics.find(
             "module `geometry.shapes` is declared inline and cannot also be "
             "defined in a separate file") != std::string::npos,
         "expected inline/external conflict diagnostic");
  expect(report->diagnostics.find("inline submodule declaration") !=
             std::string::npos,
         "expected related inline declaration note");
}

/// Verify in-session import resolution across plain, alias, group, wildcard,
/// and child imports.
auto test_compile_sources_resolves_session_imports() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto util_source = temp.path / "package_tools_util.kira";
  auto parse_source = temp.path / "package_tools_parse.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source, "module package\n"
                             "module tools\n");
  write_file(tools_source, "module package.tools\n"
                           "module util\n"
                           "module parse\n"
                           "module app\n"
                           "pub def helper() -> int32:\n"
                           "  return 1\n");
  write_file(util_source, "module package.tools.util\n"
                          "pub def shared_value() -> int32:\n"
                          "  return 2\n");
  write_file(parse_source, "module package.tools.parse\n"
                           "pub def parse_it() -> int32:\n"
                           "  return 3\n");
  write_file(app_source, "module package.tools.app\n"
                         "use package.tools\n"
                         "use package.tools.*\n"
                         "pub def run() -> int32:\n"
                         "  return 4\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  util_source.string(), parse_source.string(),
                  app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected in-session imports to return a report");
  expect(report->error_count == 0,
         "expected in-session imports to resolve cleanly");
  expect(report->modules.size() == 5, "expected all modules to emit metadata");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for valid session imports");
}

/// Verify that the real `std.io`/`std.console` source under `src/std/`
/// parses and typechecks cleanly through the real compile pipeline —
/// guards against regressing spec/stdlib.md's stdlib-source checklist item.
auto test_compile_sources_typechecks_stdlib_io_and_console() -> void {
  auto metadata_dir = make_temp_dir().path / "meta";

  auto cfg = kira::driver::cli_config{
      .program_name = "kira",
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };
  // `io.kira`'s `impl from[...]`/`impl drop for file` rely on the real
  // `from`/`drop` traits the auto-injected prelude provides, and
  // `prelude.kira` itself now `use`s `std.console`/`std.iter` — mirror what
  // `main.cpp` does for every real invocation (this alone now pulls in
  // `traits.kira`, `iter.kira`, `prelude.kira`, `io.kira`, `console.kira`,
  // `fmt.kira`, `derive.kira`, `fs/path.kira`, and the assembled
  // `std.platform`) rather than
  // hand-listing sources, which would double-add `io.kira`/`console.kira`
  // under a different path string and trip a duplicate-module-path
  // diagnostic (`find_stdlib_source_file`'s resolved path doesn't lexically
  // match a literal `"src/std/io.kira"` under `bazel test`'s runfiles tree).
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected stdlib source to return a report");
  expect(report->error_count == 0, "expected stdlib source to typecheck "
                                   "cleanly: " +
                                       report->diagnostics);
  expect(report->modules.size() == 10,
         "expected std.io, std.console, std.traits, std.iter, std.fmt, "
         "std.string, std.derive, std.fs.path, std.platform, and prelude to "
         "all emit metadata");
}

/// Verify that module-local semantic scopes reject duplicate declaration names.
auto test_compile_sources_reports_duplicate_module_scope_symbol() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "duplicate_scope.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample.tools\n"
                          "type point = int32\n"
                          "trait point:\n"
                          "  def show(self) -> str\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected duplicate declaration scope compile to return a report");
  expect(
      report->error_count > 0,
      "expected duplicate declaration names to fail semantic scope validation");
  expect(report->modules.empty(),
         "expected duplicate declaration names to block metadata output");
  expect(report->diagnostics.find(
             "duplicate declaration name `point` in module `sample.tools`") !=
             std::string::npos,
         "expected duplicate module-scope declaration diagnostic");
  expect(report->diagnostics.find("previous type declaration") !=
             std::string::npos,
         "expected note for the original declaration");
}

/// Verify that inline submodules get their own semantic declaration scopes.
auto test_compile_sources_reports_duplicate_inline_submodule_scope_symbol()
    -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "inline_duplicate_scope.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "module shapes:\n"
                          "  type circle = float64\n"
                          "  concept circle[T]:\n"
                          "    T: show\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected duplicate inline scope compile to return a report");
  expect(report->error_count > 0,
         "expected duplicate inline submodule declaration names to fail");
  expect(report->modules.empty(),
         "expected duplicate inline submodule names to block metadata output");
  expect(report->diagnostics.find(
             "duplicate declaration name `circle` in module `sample.shapes`") !=
             std::string::npos,
         "expected duplicate inline submodule declaration diagnostic");
}

/// Verify that child modules may use `super` to name parent-owned types.
auto test_compile_sources_resolves_super_qualified_type_paths() -> void {
  auto temp = make_temp_dir();
  auto geometry_source = temp.path / "geometry.kira";
  auto transform_source = temp.path / "geometry_transform.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(geometry_source, "module geometry\n"
                              "module shapes:\n"
                              "  pub type circle = { pub radius: float64 }\n"
                              "module transform\n");
  write_file(transform_source,
             "module geometry.transform\n"
             "pub def rotate(p: super.shapes.circle) -> super.shapes.circle:\n"
             "  return p\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {geometry_source.string(), transform_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected super-qualified type paths to return a report");
  expect(report->error_count == 0,
         "expected legal super-qualified type paths to resolve cleanly");
  expect(report->modules.size() == 2,
         "expected both geometry modules to emit metadata");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for legal super-qualified type paths");
}

/// Verify that unresolved qualified type paths fail semantic resolution.
auto test_compile_sources_reports_unresolved_qualified_type_path() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source, "module package\n"
                             "module tools\n");
  write_file(tools_source, "module package.tools\n"
                           "module app\n");
  write_file(app_source, "module package.tools.app\n"
                         "pub def run(value: package.tools.missing) -> int:\n"
                         "  return 1\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected unresolved qualified type path compile to return a report");
  expect(report->error_count > 0,
         "expected unresolved qualified type path to fail semantic resolution");
  expect(report->modules.size() == 2,
         "expected unaffected modules to still emit metadata");
  expect(report->diagnostics.find(
             "qualified type path `package.tools.missing` does not resolve "
             "from module `package.tools.app`") != std::string::npos,
         "expected unresolved qualified type path diagnostic");
}

/// Verify that unresolved module-qualified references fail semantic resolution.
auto test_compile_sources_reports_unresolved_module_qualified_reference()
    -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source, "module package\n"
                             "module tools\n");
  write_file(tools_source, "module package.tools\n"
                           "module app\n");
  write_file(app_source, "module package.tools.app\n"
                         "pub def run() -> int:\n"
                         "  package.tools.missing\n"
                         "  return 1\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected unresolved module-qualified reference "
                             "compile to return a report");
  expect(report->error_count > 0, "expected unresolved module-qualified "
                                  "reference to fail semantic resolution");
  expect(report->modules.size() == 2,
         "expected unaffected modules to still emit metadata");
  expect(report->diagnostics.find(
             "module-qualified reference `package.tools.missing` does not "
             "resolve from module `package.tools.app`") != std::string::npos,
         "expected unresolved module-qualified reference diagnostic");
}

/// Verify that unresolved imports fail only when the root module belongs to the
/// session.
auto test_compile_sources_reports_unresolved_session_import() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source, "module package\n"
                             "module tools\n");
  write_file(tools_source, "module package.tools\n"
                           "module app\n");
  write_file(app_source, "module package.tools.app\n"
                         "use package.tools.missing\n"
                         "use std.io\n"
                         "pub def run() -> int32:\n"
                         "  return 1\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected unresolved in-session import to return a report");
  expect(report->error_count == 1,
         "expected only the in-session unresolved import to fail");
  expect(report->modules.size() == 2,
         "expected unaffected session modules to still emit metadata");
  expect(report->diagnostics.find(
             "module `package.tools` has no member named `missing`") !=
             std::string::npos,
         "expected missing-import-member diagnostic");
  expect(report->diagnostics.find("use std.io") == std::string::npos,
         "expected external import to remain deferred without diagnostics");
}

/// Verify that in-session imports honor child-module visibility across files.
auto test_compile_sources_reports_inaccessible_session_import() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto secret_source = temp.path / "package_secret.kira";
  auto other_source = temp.path / "package_other.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source, "module package\n"
                             "module tools\n"
                             "module other\n");
  write_file(tools_source, "module package.tools\n"
                           "module secret\n");
  write_file(secret_source, "module package.tools.secret\n"
                            "pub def hidden() -> int32:\n"
                            "  return 1\n");
  write_file(other_source, "module package.other\n"
                           "use package.tools.secret\n"
                           "pub def run() -> int32:\n"
                           "  return 2\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  secret_source.string(), other_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected inaccessible in-session import to return a report");
  expect(report->error_count > 0,
         "expected inaccessible in-session import to fail");
  expect(report->modules.size() == 3,
         "expected unaffected session modules to still emit metadata");
  expect(report->diagnostics.find("module `package.tools.secret` is not "
                                  "visible from module `package.other`") !=
             std::string::npos,
         "expected inaccessible-import diagnostic");
  expect(report->diagnostics.find("restricted module declaration") !=
             std::string::npos,
         "expected related declaration label");
}

/// `--build` links its object file against the AOT panic/heap runtime
/// archives (`find_bazel_archive`, `cli.cpp`) with a plain `cc "<obj>"
/// "<panic_archive>" "<heap_archive>" -o "<out>"` invocation — `cc` alone
/// doesn't pull in libc++, so any program whose object file actually
/// references a symbol from either archive (any heap type, checked-
/// arithmetic panic, or intrinsic call) used to fail to link with pages of
/// undefined `std::__1::*`/`operator new`/`__cxa_*` symbols. Regression
/// test for switching that invocation to `c++`: builds a struct-returning
/// program (forces a real `kira_rt_alloc` reference, so this is a
/// meaningful link, not one the linker trivially no-ops because nothing in
/// the object file needs either archive), then actually runs the produced
/// executable as a real child process and checks its exit code — proving
/// this is a real, linkable, runnable native binary, not just that
/// `--build` reported success.
auto test_build_links_and_runs_a_heap_using_program() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_struct.kira";
  auto metadata_dir = temp.path / "meta";
  auto output_path = temp.path / "sample_struct_bin";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32, y: int32 }\n"
                          "def main() -> int32:\n"
                          "  let p = point { x: 1, y: 41 }\n"
                          "  return p.x + p.y\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .build = true,
      .build_function = "main",
      .build_output = output_path.string(),
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->build.has_value(), "expected a build outcome to be recorded");
  expect(report->build->succeeded,
         std::format("expected `--build` to link successfully: {}",
                     report->build->message));
  expect(fs::exists(output_path), "expected a linked executable to be written");

  const auto exit_status = std::system(output_path.string().c_str());
  expect(exit_status != -1, "expected the linked executable to launch");
#ifdef WEXITSTATUS
  expect(WEXITSTATUS(exit_status) == 42,
         "expected main()'s point{x:1,y:41}.x + .y == 42");
#endif
}

/// Same program as `test_build_links_and_runs_a_heap_using_program`, built
/// at `-O2` instead of the default `-O0` — proves `cli_config::opt_level`
/// actually reaches `llvm_codegen::emit_object_file`'s
/// `optimize_module` call and that a real `llvm::PassBuilder` pipeline
/// still produces a correct, linkable, runnable binary for a heap-using
/// program (struct construction/field access), not just a scalar one.
auto test_build_at_o2_still_links_and_runs_correctly() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_struct_o2.kira";
  auto metadata_dir = temp.path / "meta";
  auto output_path = temp.path / "sample_struct_o2_bin";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32, y: int32 }\n"
                          "def main() -> int32:\n"
                          "  let p = point { x: 1, y: 41 }\n"
                          "  return p.x + p.y\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .build = true,
      .build_function = "main",
      .build_output = output_path.string(),
      .opt_level = kira::driver::optimization_level::o2,
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->build.has_value(), "expected a build outcome to be recorded");
  expect(report->build->succeeded,
         std::format("expected `-O2 --compile` to link successfully: {}",
                     report->build->message));
  expect(fs::exists(output_path), "expected a linked executable to be written");

  const auto exit_status = std::system(output_path.string().c_str());
  expect(exit_status != -1, "expected the linked executable to launch");
#ifdef WEXITSTATUS
  expect(WEXITSTATUS(exit_status) == 42,
         "expected -O2's main()'s point{x:1,y:41}.x + .y to still be 42");
#endif
}

/// Regression test for `spec/string-formatting-design.md` on the AOT/LLVM
/// backend specifically (the bytecode VM path is covered by
/// `src/testdata/std_test/string_interpolation.kira`): builds and actually
/// runs a program using several format styles, and checks its real stdout —
/// this is what caught two real bugs during development (an `if`/`else`
/// *expression* yielding `str` failing LLVM codegen, and a `usize` literal
/// passed where `std.fmt`'s `fmt_radix_i64` declares a `uint8` parameter),
/// neither of which a VM-only or exit-code-only test would have surfaced.
auto test_build_links_and_runs_a_string_interpolation_program() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_interp.kira";
  auto metadata_dir = temp.path / "meta";
  auto output_path = temp.path / "sample_interp_bin";

  write_file(source_path, "module sample\n"
                          "def main() -> int32:\n"
                          "  let name = \"Alice\"\n"
                          "  println(\"Hi, {name}!\")\n"
                          "  println(\"Hex: {255 :04x}\")\n"
                          "  return 0\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .build = true,
      .build_function = "main",
      .build_output = output_path.string(),
  };
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected valid source to compile cleanly: " + report->diagnostics);
  expect(report->build.has_value(), "expected a build outcome to be recorded");
  expect(report->build->succeeded,
         std::format("expected `--build` to link successfully: {}",
                     report->build->message));
  expect(fs::exists(output_path), "expected a linked executable to be written");

  auto *pipe = popen(output_path.string().c_str(), "r"); // NOLINT
  expect(pipe != nullptr, "expected the linked executable to launch");
  auto output = std::string{};
  std::array<char, 256> buffer{};
  size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), read);
  }
  const auto close_status = pclose(pipe);
  expect(close_status == 0, "expected the linked executable to exit cleanly");
  expect(output == "Hi, Alice!\nHex: 00ff\n",
         std::format("unexpected stdout from the interpolation program: `{}`",
                     output));
}

/// `--run`'s exit code should mirror whatever the executed function
/// returned (mirroring any other process's `main` return value), and the
/// rendered summary should stay silent on a clean run unless
/// `--show-compile-details` was requested — `main.cpp`'s job is to turn
/// `run_outcome::exit_code` into the process exit status and to skip
/// printing `render_compile_summary`'s output when it comes back empty;
/// this test covers the `compile_sources`/`render_compile_summary` half of
/// that contract directly.
auto test_run_reports_exit_code_and_silent_summary() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_exit.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "def main() -> int32:\n"
                          "  return 42\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 42,
         "expected the run's exit code to match main()'s returned value");

  auto quiet_summary = kira::driver::render_compile_summary(*report);
  expect(quiet_summary.empty(),
         "expected a clean run's summary to be silent by default");

  auto detailed_summary = kira::driver::render_compile_summary(*report, true);
  expect(detailed_summary.find("main() -> 42") != std::string::npos,
         "expected --show-compile-details to include the run's return value");
}

/// A contract the compiler can neither prove nor refute is enforced where it
/// always could be: at run time. `--no-contract-checks` is the one thing that
/// takes that enforcement away — the spec's release elision, and the
/// programmer's assertion that the contract holds by other means.
auto test_run_enforces_unproven_contract_unless_disabled() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_contract.kira";
  auto metadata_dir = temp.path / "meta";

  // `opaque` hides the argument from the reasoning solver, so the call is
  // neither proved nor refuted at compile time.
  write_file(source_path, "module sample\n"
                          "def opaque(x: int32) -> int32:\n"
                          "  return x\n"
                          "def half(x: int32) -> int32\n"
                          "pre x >= 0, \"x must be non-negative\"\n"
                          ": x / 2\n"
                          "def main() -> int32:\n"
                          "  return half(opaque(0 - 8))\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto checked = kira::driver::compile_sources(cfg, false);
  expect(checked.has_value(), "expected compile driver to return a report");
  expect(checked->error_count == 0,
         "expected an unprovable contract to compile cleanly: " +
             checked->diagnostics);
  expect(checked->run.has_value(), "expected a run outcome to be recorded");
  expect(!checked->run->succeeded,
         "expected the violated precondition to panic at run time");
  expect(checked->run->message.find("precondition violated") !=
             std::string::npos,
         "expected the panic to name the broken precondition: " +
             checked->run->message);

  cfg.contract_checks = false;
  auto elided = kira::driver::compile_sources(cfg, false);
  expect(elided.has_value(), "expected compile driver to return a report");
  expect(elided->run.has_value(), "expected a run outcome to be recorded");
  expect(elided->run->succeeded,
         "expected `--no-contract-checks` to drop the check entirely, letting "
         "the same call run: " +
             elided->run->message);
  expect(elided->run->exit_code == -4,
         "expected the unchecked call to compute half(-8) == -4");
}

/// M4 end-to-end check: a `static let` bound to a quoted expression
/// (`` `(...)` ``), spliced back into `main`'s body with `~`, must actually
/// execute the quoted arithmetic under `--run` — not just type-check. This
/// is the first runnable program exercising quote/splice reification
/// (`checker::infer_expr`'s `splice_expr` case grafting the fragment,
/// `hir::lower`'s new `splice_expr` case following `spliced_fragments`).
auto test_run_executes_spliced_quoted_expression() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_splice.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "static let doubled: expr = `(21 + 21)`\n"
                          "def main() -> int32:\n"
                          "  return ~doubled\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a spliced quoted expression to compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 42,
         "expected `~doubled` to splice in `21 + 21` and actually execute "
         "it, returning 42");
}

/// M4.5 end-to-end check: `expr.lit(42)` — the AST-builder intrinsic that
/// programmatically constructs a new `expr` quote value, rather than
/// capturing existing syntax with `` `(...)` `` — must actually execute
/// once spliced back in, mirroring `test_run_executes_spliced_quoted_
/// expression` for M4's backtick-quote case.
auto test_run_executes_spliced_builder_constructed_expression() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_builder_splice.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "static let built: expr = expr.lit(42)\n"
                          "def main() -> int32:\n"
                          "  return ~built\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a spliced `expr.lit`-built expression to compile "
         "cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 42,
         "expected `~built` to splice in the `expr.lit(42)`-constructed "
         "fragment and actually execute it, returning 42");
}

/// M4.6 end-to-end check: `~make_show_impl()` at module top level — item-
/// level splice — must inject a real `impl show for point` that later
/// checking, method lookup, and lowering all see, so `p.show()` actually
/// executes the spliced method's body under `--run`. Proves `semantic::
/// checker::resolve_item_splices` (evaluated before `build_method_table`/
/// `validate_impl_coherence`, so the injected impl participates in
/// coherence/method-table bookkeeping exactly like one written directly in
/// source) and `hir::lower_module`'s new `synthesized_item_splices` walk.
auto test_run_executes_item_level_splice_injected_impl() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_item_splice.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32 }\n"
                          "trait show:\n"
                          "    def show(self) -> int32\n"
                          "static def make_show_impl() -> def_expr:\n"
                          "    return `(impl show for point:\n"
                          "        def show(self) -> int32:\n"
                          "            return 42)`\n"
                          "~make_show_impl()\n"
                          "def main() -> int32:\n"
                          "  let p = point{x: 1}\n"
                          "  return p.show()\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected an item-level splice injecting `impl show for point` to "
         "compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 42,
         "expected `p.show()` to call the item-splice-injected method and "
         "actually execute it, returning 42");
}

/// A lambda whose body is a string interpolation capturing an outer local.
/// Interpolation desugars into calls to `std.fmt` helpers whose callees are
/// `hir_local_ref`s to module-level functions; `free_variables` reports those
/// globals alongside the genuinely-captured `prefix`, and both backends must
/// filter the globals out of the closure environment (otherwise lowering fails
/// with "a captured variable could not be found"). Needs the full `std`
/// session (unlike the std-free corpus sibling `036_lambda_captures_local_not_
/// global.kira`), so it runs here through `compile_sources`. `g(42)` yields
/// "n=42", whose length is 4.
auto test_run_lambda_body_string_interpolation_captures() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_lambda_interp.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "def apply(f: fn(int32) -> str, x: int32) -> int32:\n"
                          "    return f(x).len() as int32\n"
                          "def main() -> int32:\n"
                          "    let prefix = \"n=\"\n"
                          "    let g: fn(int32) -> str = k => \"{prefix}{k}\"\n"
                          "    return apply(g, 42)\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };
  // String interpolation desugars against `std.fmt`, so this program needs the
  // auto-imported prelude/stdlib that `main.cpp` injects for real invocations
  // (`compile_sources` compiles exactly the sources it is handed).
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a lambda whose body interpolates a captured local to "
         "compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded,
         "expected `main` to run without panicking: " + report->run->message);
  expect(report->run->exit_code == 4,
         "expected the interpolating lambda to render \"n=42\" (length 4), "
         "capturing `prefix` while resolving the `std.fmt` helpers directly");
}

/// End-to-end check that a hand-rolled type implementing the *real*
/// `std.iter.iterator[T]` trait drives a `for` loop. `check_body_node`'s
/// `for_stmt` case records the resolved `next` dispatch and `hir::lower_
/// iterator_loop` desugars `for x in it: ...` into `while let @some(x) =
/// it.next(): ...`; `next(mut self)` mutates the handle each pass. Needs the
/// full `std` session for `std.iter`, so it runs here (unlike the std-free
/// corpus sibling `037_user_iterator_for_loop.kira`, which defines its own
/// `iterator` trait). Sum of 0..5 == 10.
auto test_run_for_loop_over_user_std_iterator() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_user_iterator.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "use std.iter.iterator\n"
                          "type counter = { current: int32, limit: int32 }\n"
                          "impl iterator[int32] for counter:\n"
                          "    def next(mut self) -> option[int32]:\n"
                          "        if self.current >= self.limit:\n"
                          "            return @none\n"
                          "        let value = self.current\n"
                          "        self.current = self.current + 1\n"
                          "        return @some(value)\n"
                          "def main() -> int32:\n"
                          "    var it = counter { current: 0, limit: 5 }\n"
                          "    var total = 0\n"
                          "    for x in it:\n"
                          "        total = total + x\n"
                          "    return total\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a for-loop over a user `std.iter.iterator` to compile "
         "cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded,
         "expected `main` to run without panicking: " + report->run->message);
  expect(report->run->exit_code == 10,
         "expected `for x in it` over a 0..5 counter iterator to sum to 10");
}

/// A free function generic over *types* is monomorphized once per concrete
/// type its call sites solve for (`identity$int32`, `choose$int32`) and then
/// runs — the template that used to fail lowering wholesale now reaches a
/// backend through its instances (`instantiate_type_generic`).
auto test_run_type_generic_free_function() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_type_generic.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "def identity[T](x: T) -> T:\n"
                          "    return x\n"
                          "def choose[T](flag: bool, a: T, b: T) -> T:\n"
                          "    if flag:\n"
                          "        return a\n"
                          "    return b\n"
                          "def main() -> int32:\n"
                          "    let a = identity(7)\n"
                          "    let b = choose(true, 30, 99)\n"
                          "    let c = choose(false, 1, 3)\n"
                          "    return a + b + c\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a type-generic free function to compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded,
         "expected `main` to run without panicking: " + report->run->message);
  expect(report->run->exit_code == 40,
         "expected identity(7) + choose(true,30,99) + choose(false,1,3) = 40");
}

/// M5 end-to-end check: hygiene must prevent a spliced `let` from
/// *clobbering* a splice-site binding of the same name, not just avoid a
/// compile error. `main` binds its own `temp` to `1`, then splices a quoted
/// `let temp: int32 = 99` right after it — `check_body_node`'s `splice_stmt`
/// case reuses the enclosing scope (no push), so without hygiene the
/// spliced `let` would silently rebind `temp` in that same scope, and
/// `return temp` would come back `99`, not `1`. `comptime::
/// rename_internal_bindings` (`hygiene.cpp`) renames the fragment's own
/// `temp` to a fresh synthetic name before it's ever spliced, so the two
/// bindings coexist independently — proven here by checking the actual
/// runtime value, which a mere "compiles without error" check couldn't
/// distinguish from the old, unhygienic behavior.
auto test_run_hygiene_prevents_spliced_let_from_clobbering_splice_site()
    -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_hygiene.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "static let make_temp: stmt = `let temp: int32 = "
                          "99`\n"
                          "def main() -> int32:\n"
                          "  let temp: int32 = 1\n"
                          "  ~make_temp\n"
                          "  return temp\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected splicing a quoted `let temp = 99` next to an existing "
         "`temp` binding to compile cleanly under hygiene: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 1,
         "expected `main`'s own `temp` (1) to survive the spliced `let "
         "temp = 99` untouched — hygiene renamed the spliced binding, so "
         "it didn't clobber the splice site's `temp`");
}

/// M6 end-to-end check: `point.field_count()` (`comptime::
/// try_eval_type_reflection_call`, `reflect.cpp`) must actually reflect
/// `point`'s real field count at compile time, and that value must reach a
/// runnable program — routed here through `expr.lit(...)` (M4.5) +
/// `~splice` (M4), the same proven "compile-time value becomes real code"
/// path `test_run_executes_spliced_builder_constructed_expression` already
/// exercises, with reflection just supplying the argument. (A bare scalar
/// `static let` referenced by *plain name*, with no splice at all, is a
/// separate path — see `test_run_scalar_static_let_referenced_by_name`.)
auto test_run_reflects_struct_field_count_into_runtime_constant() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_reflect.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32, y: int32, z: int32 }\n"
                          "static let count_expr: expr = "
                          "expr.lit(point.field_count())\n"
                          "def main() -> int32:\n"
                          "  return ~count_expr\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected `expr.lit(point.field_count())` spliced into `main` to "
         "compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 3,
         "expected `point.field_count()` to reflect `point`'s real 3 "
         "fields (x, y, z) and for that value to reach `main` at runtime "
         "via the spliced literal");
}

/// A scalar `static let` referenced by plain name (no `~splice`, no
/// `expr`/`stmt` quote type at all — just an ordinary `int32` constant)
/// from an unrelated runtime function used to fail at *bytecode
/// compilation*, not at type-checking: `hir::lower_ident` had no route to
/// a runtime representation for it (neither a real local — no `let`/
/// parameter ever declared one — nor a function), so it emitted an
/// unresolvable `hir_local_ref` and `bytecode_compiler` rejected it with
/// "reference to `count` is not a local binding". `semantic::checker::
/// resolve_ident` now evaluates the binding eagerly (via `comptime::
/// evaluator`, exactly like any other compile-time constant) and records
/// a synthesized literal for `hir::lower_ident` to embed directly — this
/// is the fix, proven end-to-end here rather than just documented as a
/// known gap. `point.field_count()` doubles as the value source so this
/// also covers reflection reaching runtime through the *simpler*, more
/// natural path (no explicit `expr.lit`/`~splice` needed at all).
auto test_run_scalar_static_let_referenced_by_name() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_scalar_static_let.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32, y: int32, z: int32 }\n"
                          "static let count: int32 = point.field_count()\n"
                          "def main() -> int32:\n"
                          "  return count\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .run = true,
      .run_function = "main",
  };

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected a scalar `static let count: int32 = point.field_count()` "
         "referenced by plain name from `main` to compile cleanly: " +
             report->diagnostics);
  expect(report->run.has_value(), "expected a run outcome to be recorded");
  expect(report->run->succeeded, "expected `main` to run without panicking");
  expect(report->run->exit_code == 3,
         "expected `return count` to embed `count`'s compile-time-evaluated "
         "value (3) directly, with no splice syntax required");
}

/// M7 end-to-end check: `type ... deriving show` on a concrete struct now
/// gets a *real*, dynamically-derived `show()` method — not just a type
/// that happens to check `p.show(): str` with no runtime body, which is all
/// `deriving show` ever did before this milestone (`checker::
/// derived_method_result` only ever provided a result *type*, and the one
/// existing runtime path for it — string interpolation's implicit `.show()`
/// dispatch — explicitly refused to lower it: "this type's capability comes
/// only from `deriving`, which has no runtime body yet"). `checker::
/// resolve_deriving_traits` now splices `~derive_show[point]()`
/// (`src/std/deriving.kira`, module `std.derive`) in behind the scenes for
/// every concrete, struct-shaped `deriving show`, so `p.show()` here calls
/// a method built at
/// compile time from `point`'s own real field list via reflection
/// (`T.fields()`) and the AST-builder intrinsics — proven by checking the
/// program's actual stdout, not just its exit code, since only the real
/// formatted text (not just "did it compile") distinguishes a working
/// derivation from a lucky compile.
auto test_build_derives_show_via_deriving_clause() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_derive_show.kira";
  auto metadata_dir = temp.path / "meta";
  auto output_path = temp.path / "sample_derive_show_bin";

  write_file(source_path, "module sample\n"
                          "type point = { x: int32, y: int32 } deriving show\n"
                          "def main() -> int32:\n"
                          "  let p: point = { x: 3, y: 4 }\n"
                          "  println(p.show())\n"
                          "  return 0\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .build = true,
      .build_function = "main",
      .build_output = output_path.string(),
  };
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected `type point = {...} deriving show` to compile cleanly: " +
             report->diagnostics);
  expect(report->build.has_value(), "expected a build outcome to be recorded");
  expect(report->build->succeeded,
         std::format("expected `--build` to link successfully: {}",
                     report->build->message));
  expect(fs::exists(output_path), "expected a linked executable to be written");

  auto *pipe = popen(output_path.string().c_str(), "r"); // NOLINT
  expect(pipe != nullptr, "expected the linked executable to launch");
  auto output = std::string{};
  std::array<char, 256> buffer{};
  size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), read);
  }
  const auto close_status = pclose(pipe);
  expect(close_status == 0, "expected the linked executable to exit cleanly");
  expect(
      output == "point { x: 3, y: 4 }\n",
      std::format("unexpected stdout from the derived `show()`: `{}`", output));
}

/// M7 follow-up ("make everything use the new style"): `eq` and `debug`
/// re-derived for real too, the same way `show` was — `checker::
/// resolve_deriving_traits` now splices in `derive_eq`/`derive_debug`
/// (`src/std/deriving.kira`) alongside `derive_show` for every trait named
/// in `deriving` that has one. `derive_eq`'s generated `def eq(self, other:
/// &self) -> bool` is what caught a real, independent, pre-existing bug in
/// *both* backends: field access through a `&self`-typed reference operand
/// (`other.x`) failed with "field access `.x` is only supported on struct
/// values" — confirmed with a hand-written `impl eq` with no comptime
/// involvement at all, so this wasn't specific to generated code. Root
/// cause: `bytecode_compiler::compile_field`/`llvm_codegen::compile_field`
/// looked up the field's byte offset directly against `field.object`'s
/// recorded type, which still carries the unstripped `&T` the checker
/// records (per `strip_refs`'s own doc comment in both files) — every other
/// call site already stripped refs before a type-kind-sensitive lookup;
/// these two didn't. Fixed by routing the `runtime::struct_field_offset`
/// lookup through `strip_refs` in both. `ord`/`hash` are deliberately not
/// covered here — `ordering` has no real variants or runtime representation
/// anywhere yet, and no builtin scalar implements `.hash()`; both still use
/// the old type-check-only `derived_method_result` path.
auto test_build_derives_eq_and_debug_via_deriving_clause() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "sample_derive_eq_debug.kira";
  auto metadata_dir = temp.path / "meta";
  auto output_path = temp.path / "sample_derive_eq_debug_bin";

  write_file(source_path,
             "module sample\n"
             "type point = { x: int32, y: int32 } deriving show, eq, debug\n"
             "def main() -> int32:\n"
             "  let a: point = { x: 3, y: 4 }\n"
             "  let b: point = { x: 3, y: 4 }\n"
             "  let c: point = { x: 3, y: 5 }\n"
             "  println(a.debug())\n"
             "  if a.eq(&b) and not a.eq(&c):\n"
             "    return 42\n"
             "  return 0\n");

  kira::driver::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
      .build = true,
      .build_function = "main",
      .build_output = output_path.string(),
  };
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0,
         "expected `deriving show, eq, debug` to compile cleanly: " +
             report->diagnostics);
  expect(report->build.has_value(), "expected a build outcome to be recorded");
  expect(report->build->succeeded,
         std::format("expected `--build` to link successfully: {}",
                     report->build->message));
  expect(fs::exists(output_path), "expected a linked executable to be written");

  auto *pipe = popen(output_path.string().c_str(), "r"); // NOLINT
  expect(pipe != nullptr, "expected the linked executable to launch");
  auto output = std::string{};
  std::array<char, 256> buffer{};
  size_t read = 0;
  while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
    output.append(buffer.data(), read);
  }
  const auto close_status = pclose(pipe);
  expect(output == "point { x: 3, y: 4 }\n",
         std::format("unexpected stdout from the derived `debug()`: `{}`",
                     output));
#ifdef WEXITSTATUS
  expect(WEXITSTATUS(close_status) == 42,
         "expected `a.eq(&b)` true and `a.eq(&c)` false from the derived "
         "`eq()`, i.e. exit code 42");
#endif
}

} // namespace

/// Run the CLI driver regression tests.
auto main() -> int {
  try {
    test_parse_args_accepts_sources();
    test_parse_args_supports_help_and_double_dash();
    test_parse_args_supports_version();
    test_parse_args_accepts_metadata_dir();
    test_parse_args_rejects_unknown_options();
    test_parse_args_accepts_show_compile_details();
    test_parse_args_accepts_optimization_level();
    test_rendering_helpers();
    test_compile_sources_writes_module_metadata();
    test_compile_sources_writes_functor_instantiation_metadata();
    test_compile_sources_folds_static_if_import_selection();
    test_compile_sources_rejects_use_gated_by_nonliteral_static_if();
    test_compile_sources_lowers_module_to_hir();
    test_compile_sources_records_hir_lowering_failure_without_failing_compile();
    test_compile_sources_skips_lowering_when_parse_only();
    test_compile_sources_reports_parser_errors();
    test_compile_sources_reports_nested_parser_errors();
    test_compile_sources_handles_multiple_files();
    test_compile_sources_reports_duplicate_module_paths();
    test_compile_sources_accepts_declared_external_submodule();
    test_compile_sources_reports_missing_parent_submodule_declaration();
    test_compile_sources_reports_inline_external_submodule_conflict();
    test_compile_sources_resolves_session_imports();
    test_compile_sources_typechecks_stdlib_io_and_console();
    test_compile_sources_reports_duplicate_module_scope_symbol();
    test_compile_sources_reports_duplicate_inline_submodule_scope_symbol();
    test_compile_sources_resolves_super_qualified_type_paths();
    test_compile_sources_reports_unresolved_qualified_type_path();
    test_compile_sources_reports_unresolved_module_qualified_reference();
    test_compile_sources_reports_unresolved_session_import();
    test_compile_sources_reports_inaccessible_session_import();
    test_build_links_and_runs_a_heap_using_program();
    test_build_at_o2_still_links_and_runs_correctly();
    test_build_links_and_runs_a_string_interpolation_program();
    test_run_reports_exit_code_and_silent_summary();
    test_run_enforces_unproven_contract_unless_disabled();
    test_run_executes_spliced_quoted_expression();
    test_run_executes_spliced_builder_constructed_expression();
    test_run_executes_item_level_splice_injected_impl();
    test_run_lambda_body_string_interpolation_captures();
    test_run_for_loop_over_user_std_iterator();
    test_run_type_generic_free_function();
    test_run_hygiene_prevents_spliced_let_from_clobbering_splice_site();
    test_run_reflects_struct_field_count_into_runtime_constant();
    test_run_scalar_static_let_referenced_by_name();
    test_build_derives_show_via_deriving_clause();
    test_build_derives_eq_and_debug_via_deriving_clause();
  } catch (const std::exception &ex) {
    std::cerr << "cli_test failed with exception: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
