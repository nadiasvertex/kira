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
  expect(metadata.schema_version() == 1, "expected metadata schema version");
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
  expect(metadata.dependencies(0).fields().at("version") == "3.45",
         "expected dependency field value to be unquoted");
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
  // `prelude.kira` itself now `use`s `std.console` — mirror what
  // `main.cpp` does for every real invocation (this alone now pulls in
  // `traits.kira`, `prelude.kira`, `io.kira`, `console.kira`, and
  // `fmt.kira`) rather than hand-listing sources, which would double-add
  // `io.kira`/`console.kira` under a different path string and trip a
  // duplicate-module-path diagnostic (`find_stdlib_source_file`'s resolved
  // path doesn't lexically match a literal `"src/std/io.kira"` under `bazel
  // test`'s runfiles tree).
  kira::driver::inject_stdlib_prelude(cfg);

  auto report = kira::driver::compile_sources(cfg, false);
  expect(report.has_value(), "expected stdlib source to return a report");
  expect(report->error_count == 0, "expected stdlib source to typecheck "
                                   "cleanly: " +
                                       report->diagnostics);
  expect(report->modules.size() == 5,
         "expected std.io, std.console, std.traits, std.fmt, and prelude to "
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
  expect(report->diagnostics.find("import `package.tools.missing` does not "
                                  "resolve in this compilation session") !=
             std::string::npos,
         "expected unresolved in-session import diagnostic");
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

  write_file(source_path,
             "module sample\n"
             "type point = { x: int32 }\n"
             "trait show:\n"
             "    def show(self) -> int32\n"
             "static def make_show_impl() -> def_expr:\n"
             "    return `impl show for point:\n"
             "        def show(self) -> int32:\n"
             "            return 42\n"
             "    `\n"
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

} // namespace

/// Run the CLI driver regression tests.
auto main() -> int {
  try {
    test_parse_args_accepts_sources();
    test_parse_args_supports_help_and_double_dash();
    test_parse_args_accepts_metadata_dir();
    test_parse_args_rejects_unknown_options();
    test_parse_args_accepts_show_compile_details();
    test_parse_args_accepts_optimization_level();
    test_rendering_helpers();
    test_compile_sources_writes_module_metadata();
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
    test_run_executes_spliced_quoted_expression();
    test_run_executes_spliced_builder_constructed_expression();
    test_run_executes_item_level_splice_injected_impl();
  } catch (const std::exception &ex) {
    std::cerr << "cli_test failed with exception: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
