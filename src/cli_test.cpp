#include "cli.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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

/// Verify that plain source paths are accepted as positional arguments.
auto test_parse_args_accepts_sources() -> void {
  char arg0[] = "kira";
  char arg1[] = "main.kira";
  char arg2[] = "lib.kira";
  char *argv[] = {arg0, arg1, arg2};

  auto result = kira::parse_args(3, argv);
  expect(result.has_value(), "expected sources to parse successfully");
  expect(!result->show_help, "plain sources should not request help");
  expect(result->sources.size() == 2, "expected two source arguments");
  expect(result->sources[0] == "main.kira", "expected first source path");
  expect(result->sources[1] == "lib.kira", "expected second source path");
  expect(result->metadata_dir == kira::kDefaultMetadataDir,
         "expected default metadata directory");
}

/// Verify help handling and `--` option termination.
auto test_parse_args_supports_help_and_double_dash() -> void {
  char arg0[] = "kira";
  char arg1[] = "--help";
  char *help_argv[] = {arg0, arg1};

  auto help_result = kira::parse_args(2, help_argv);
  expect(help_result.has_value(), "help should parse successfully");
  expect(help_result->show_help, "--help should set show_help");

  char arg2[] = "--";
  char arg3[] = "-literal.kira";
  char *dash_argv[] = {arg0, arg2, arg3};

  auto dash_result = kira::parse_args(3, dash_argv);
  expect(dash_result.has_value(), "-- should stop option parsing");
  expect(dash_result->sources.size() == 1, "expected one source after --");
  expect(dash_result->sources[0] == "-literal.kira",
         "expected source after -- to be preserved");
}

/// Verify that the metadata output directory can be overridden.
auto test_parse_args_accepts_metadata_dir() -> void {
  char arg0[] = "kira";
  char arg1[] = "--metadata-dir";
  char arg2[] = "build/meta";
  char arg3[] = "main.kira";
  char *argv[] = {arg0, arg1, arg2, arg3};

  auto result = kira::parse_args(4, argv);
  expect(result.has_value(), "metadata dir option should parse successfully");
  expect(result->metadata_dir == "build/meta",
         "expected metadata directory override");
}

/// Verify that unknown command-line options are rejected.
auto test_parse_args_rejects_unknown_options() -> void {
  char arg0[] = "kira";
  char arg1[] = "--bogus";
  char *argv[] = {arg0, arg1};

  auto result = kira::parse_args(2, argv);
  expect(!result.has_value(), "unknown options should fail");
  expect(result.error() == "unknown option: --bogus",
         "expected unknown option error message");
}

/// Verify help and compile-summary rendering helpers.
auto test_rendering_helpers() -> void {
  auto help = kira::render_help("kira");
  expect(help.find("Usage: kira [OPTIONS] SOURCES...") != std::string::npos,
         "help should contain usage line");
  expect(help.find("Kira - Parse source files and emit module metadata") !=
              std::string::npos,
          "help should contain description");
  expect(help.find("--metadata-dir PATH") != std::string::npos,
         "help should document metadata dir option");

  kira::compile_report report{
      .modules = {{
          .source_path = "main.kira",
          .module_path = {"sample", "tools"},
          .metadata_path = "build/meta/sample/tools.kmeta.pb",
      }},
      .diagnostics = {},
      .error_count = 0,
  };
  auto summary = kira::render_compile_summary(report);
  expect(summary.find("Compiled 1 module(s):") != std::string::npos,
         "summary should report compiled module count");
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
  auto base = fs::temp_directory_path() /
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

  write_file(source_path,
             "module sample.tools\n"
             "no_prelude\n"
             "use std.io\n"
             "dep sqlite:\n"
             "  version = \"3.45\"\n"
             "pub def run() -> int32:\n"
             "  return 1\n"
             "type person = { name: str }\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to return a report");
  expect(report->error_count == 0, "expected valid source to compile cleanly");
  expect(report->modules.size() == 1, "expected one metadata artifact");
  expect(report->diagnostics.empty(), "expected no diagnostics for valid source");

  auto metadata_path = fs::path(report->modules[0].metadata_path);
  expect(fs::exists(metadata_path), "expected metadata file to be written");

  auto in = std::ifstream(metadata_path, std::ios::binary);
  expect(static_cast<bool>(in), "expected metadata file to open");

  auto metadata = kira::metadata::v1::ModuleMetadata{};
  expect(metadata.ParseFromIstream(&in), "expected metadata protobuf to parse");
  expect(metadata.schema_version() == 1, "expected metadata schema version");
  expect(metadata.module_path_size() == 2, "expected module path components");
  expect(metadata.module_path(0) == "sample", "expected first module path part");
  expect(metadata.module_path(1) == "tools", "expected second module path part");
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

/// Verify that parser failures are reported and block metadata output.
auto test_compile_sources_reports_parser_errors() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "broken.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path,
             "def broken():\n"
             "  return 1\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected compile driver to report parser failures");
  expect(report->error_count > 0, "expected parser errors for invalid source");
  expect(report->modules.empty(), "expected no metadata artifacts on failure");
  expect(report->diagnostics.find(
             "every Kira source file must start with a `module` declaration") !=
              std::string::npos,
          "expected missing-module diagnostic");
  expect(!fs::exists(metadata_dir), "expected no metadata directory on failure");
}

/// Verify that malformed nested blocks still terminate instead of looping.
auto test_compile_sources_reports_nested_parser_errors() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "nested_broken.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path,
             "module sample\n"
             "module util:\n"
             "  pub def shared() -> int32:\n"
             "    return 1\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
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

  write_file(source_a,
             "module sample.tools\n"
             "pub def run() -> int32:\n"
             "  return 1\n");
  write_file(source_b,
             "module sample.math\n"
             "pub def add() -> int32:\n"
             "  return 2\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_a.string(), source_b.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected multi-file compile to return a report");
  expect(report->error_count == 0, "expected multi-file compile to succeed");
  expect(report->modules.size() == 2, "expected two metadata artifacts");
  expect(report->diagnostics.empty(), "expected no diagnostics for valid inputs");
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

  write_file(source_a,
             "module sample.tools\n"
             "pub def first() -> int32:\n"
             "  return 1\n");
  write_file(source_b,
             "module sample.tools\n"
             "pub def second() -> int32:\n"
             "  return 2\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_a.string(), source_b.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected duplicate-module compile to return a report");
  expect(report->error_count > 0, "expected duplicate module path to fail");
  expect(report->modules.empty(), "expected duplicate modules to block metadata output");
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

  write_file(parent_source,
             "module geometry\n"
             "module transform\n"
             "pub def root() -> int32:\n"
             "  return 1\n");
  write_file(child_source,
             "module geometry.transform\n"
             "pub def rotate() -> int32:\n"
             "  return 2\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected declared submodule compile to return a report");
  expect(report->error_count == 0,
         "expected declared external submodule to compile cleanly");
  expect(report->modules.size() == 2,
         "expected parent and child metadata artifacts");
  expect(report->diagnostics.empty(),
         "expected no diagnostics for declared external submodule");
}

/// Verify that child modules need a matching declaration in their parent module.
auto test_compile_sources_reports_missing_parent_submodule_declaration() -> void {
  auto temp = make_temp_dir();
  auto parent_source = temp.path / "geometry.kira";
  auto child_source = temp.path / "geometry_transform.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(parent_source,
             "module geometry\n"
             "pub def root() -> int32:\n"
             "  return 1\n");
  write_file(child_source,
             "module geometry.transform\n"
             "pub def rotate() -> int32:\n"
             "  return 2\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected missing-parent compile to return a report");
  expect(report->error_count > 0,
         "expected undeclared external submodule to fail");
  expect(report->modules.empty(),
         "expected undeclared external submodule to block metadata output");
  expect(report->diagnostics.find(
             "module `geometry.transform` is not declared by parent module `geometry`") !=
             std::string::npos,
         "expected missing parent declaration diagnostic");
  expect(report->diagnostics.find("module transform") != std::string::npos,
         "expected help text to mention missing submodule declaration");
}

/// Verify that inline and separate-file definitions of the same child module conflict.
auto test_compile_sources_reports_inline_external_submodule_conflict() -> void {
  auto temp = make_temp_dir();
  auto parent_source = temp.path / "geometry.kira";
  auto child_source = temp.path / "geometry_shapes.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(parent_source,
             "module geometry\n"
             "module shapes:\n"
             "  pub type circle = { pub radius: float64 }\n");
  write_file(child_source,
             "module geometry.shapes\n"
             "pub def area() -> int32:\n"
             "  return 3\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {parent_source.string(), child_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected inline-conflict compile to return a report");
  expect(report->error_count > 0,
         "expected inline and external submodule conflict to fail");
  expect(report->modules.empty(),
         "expected inline and external submodule conflict to block metadata output");
  expect(report->diagnostics.find(
             "module `geometry.shapes` is declared inline and cannot also be defined in a separate file") !=
             std::string::npos,
         "expected inline/external conflict diagnostic");
  expect(report->diagnostics.find("inline submodule declaration") !=
             std::string::npos,
         "expected related inline declaration note");
}

/// Verify in-session import resolution across plain, alias, group, wildcard, and child imports.
auto test_compile_sources_resolves_session_imports() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto util_source = temp.path / "package_tools_util.kira";
  auto parse_source = temp.path / "package_tools_parse.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source,
             "module package\n"
             "module tools\n");
  write_file(tools_source,
             "module package.tools\n"
             "module util\n"
             "module parse\n"
             "module app\n"
             "pub def helper() -> int32:\n"
             "  return 1\n");
  write_file(util_source,
             "module package.tools.util\n"
             "pub def shared_value() -> int32:\n"
             "  return 2\n");
  write_file(parse_source,
             "module package.tools.parse\n"
             "pub def parse_it() -> int32:\n"
             "  return 3\n");
  write_file(app_source,
             "module package.tools.app\n"
             "use package.tools\n"
             "use package.tools.*\n"
             "pub def run() -> int32:\n"
             "  return 4\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  util_source.string(), parse_source.string(), app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected in-session imports to return a report");
  expect(report->error_count == 0, "expected in-session imports to resolve cleanly");
  expect(report->modules.size() == 5, "expected all modules to emit metadata");
  expect(report->diagnostics.empty(), "expected no diagnostics for valid session imports");
}

/// Verify that module-local semantic scopes reject duplicate declaration names.
auto test_compile_sources_reports_duplicate_module_scope_symbol() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "duplicate_scope.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path,
             "module sample.tools\n"
             "type point = int32\n"
             "trait point:\n"
             "  def show(self) -> str\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected duplicate declaration scope compile to return a report");
  expect(report->error_count > 0,
         "expected duplicate declaration names to fail semantic scope validation");
  expect(report->modules.empty(),
         "expected duplicate declaration names to block metadata output");
  expect(report->diagnostics.find(
             "duplicate declaration name `point` in module `sample.tools`") !=
             std::string::npos,
         "expected duplicate module-scope declaration diagnostic");
  expect(report->diagnostics.find("previous type declaration") != std::string::npos,
         "expected note for the original declaration");
}

/// Verify that inline submodules get their own semantic declaration scopes.
auto test_compile_sources_reports_duplicate_inline_submodule_scope_symbol() -> void {
  auto temp = make_temp_dir();
  auto source_path = temp.path / "inline_duplicate_scope.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(source_path,
             "module sample\n"
             "module shapes:\n"
             "  type circle = float64\n"
             "  concept circle[T]:\n"
             "    T: show\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {source_path.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
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

  write_file(geometry_source,
             "module geometry\n"
             "module shapes:\n"
             "  pub type circle = { pub radius: float64 }\n"
             "module transform\n");
  write_file(transform_source,
             "module geometry.transform\n"
             "pub def rotate(p: super.shapes.circle) -> super.shapes.circle:\n"
             "  return p\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {geometry_source.string(), transform_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected super-qualified type paths to return a report");
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

  write_file(package_source,
             "module package\n"
             "module tools\n");
  write_file(tools_source,
             "module package.tools\n"
             "module app\n");
  write_file(app_source,
             "module package.tools.app\n"
             "pub def run(value: package.tools.missing) -> int:\n"
             "  return 1\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(), app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected unresolved qualified type path compile to return a report");
  expect(report->error_count > 0,
         "expected unresolved qualified type path to fail semantic resolution");
  expect(report->modules.size() == 2,
         "expected unaffected modules to still emit metadata");
  expect(report->diagnostics.find(
             "qualified type path `package.tools.missing` does not resolve from module `package.tools.app`") !=
             std::string::npos,
         "expected unresolved qualified type path diagnostic");
}

/// Verify that unresolved module-qualified references fail semantic resolution.
auto test_compile_sources_reports_unresolved_module_qualified_reference() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source,
             "module package\n"
             "module tools\n");
  write_file(tools_source,
             "module package.tools\n"
             "module app\n");
  write_file(app_source,
             "module package.tools.app\n"
             "pub def run() -> int:\n"
             "  package.tools.missing\n"
             "  return 1\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(), app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(),
         "expected unresolved module-qualified reference compile to return a report");
  expect(report->error_count > 0,
         "expected unresolved module-qualified reference to fail semantic resolution");
  expect(report->modules.size() == 2,
         "expected unaffected modules to still emit metadata");
  expect(report->diagnostics.find(
             "module-qualified reference `package.tools.missing` does not resolve from module `package.tools.app`") !=
             std::string::npos,
         "expected unresolved module-qualified reference diagnostic");
}

/// Verify that unresolved imports fail only when the root module belongs to the session.
auto test_compile_sources_reports_unresolved_session_import() -> void {
  auto temp = make_temp_dir();
  auto package_source = temp.path / "package.kira";
  auto tools_source = temp.path / "package_tools.kira";
  auto app_source = temp.path / "package_tools_app.kira";
  auto metadata_dir = temp.path / "meta";

  write_file(package_source,
             "module package\n"
             "module tools\n");
  write_file(tools_source,
             "module package.tools\n"
             "module app\n");
  write_file(app_source,
             "module package.tools.app\n"
             "use package.tools.missing\n"
             "use std.io\n"
             "pub def run() -> int32:\n"
             "  return 1\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(), app_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected unresolved in-session import to return a report");
  expect(report->error_count == 1,
         "expected only the in-session unresolved import to fail");
  expect(report->modules.size() == 2,
         "expected unaffected session modules to still emit metadata");
  expect(report->diagnostics.find(
             "import `package.tools.missing` does not resolve in this compilation session") !=
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

  write_file(package_source,
             "module package\n"
             "module tools\n"
             "module other\n");
  write_file(tools_source,
             "module package.tools\n"
             "module secret\n");
  write_file(secret_source,
             "module package.tools.secret\n"
             "pub def hidden() -> int32:\n"
             "  return 1\n");
  write_file(other_source,
             "module package.other\n"
             "use package.tools.secret\n"
             "pub def run() -> int32:\n"
             "  return 2\n");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {package_source.string(), tools_source.string(),
                  secret_source.string(), other_source.string()},
      .metadata_dir = metadata_dir.string(),
      .show_help = false,
  };

  auto report = kira::compile_sources(cfg, false);
  expect(report.has_value(), "expected inaccessible in-session import to return a report");
  expect(report->error_count > 0, "expected inaccessible in-session import to fail");
  expect(report->modules.size() == 3,
         "expected unaffected session modules to still emit metadata");
  expect(report->diagnostics.find(
             "module `package.tools.secret` is not visible from module `package.other`") !=
             std::string::npos,
         "expected inaccessible-import diagnostic");
  expect(report->diagnostics.find("restricted module declaration") != std::string::npos,
         "expected related declaration label");
}

} // namespace

/// Run the CLI driver regression tests.
auto main() -> int {
  test_parse_args_accepts_sources();
  test_parse_args_supports_help_and_double_dash();
  test_parse_args_accepts_metadata_dir();
  test_parse_args_rejects_unknown_options();
  test_rendering_helpers();
  test_compile_sources_writes_module_metadata();
  test_compile_sources_reports_parser_errors();
  test_compile_sources_reports_nested_parser_errors();
  test_compile_sources_handles_multiple_files();
  test_compile_sources_reports_duplicate_module_paths();
  test_compile_sources_accepts_declared_external_submodule();
  test_compile_sources_reports_missing_parent_submodule_declaration();
  test_compile_sources_reports_inline_external_submodule_conflict();
  test_compile_sources_resolves_session_imports();
  test_compile_sources_reports_duplicate_module_scope_symbol();
  test_compile_sources_reports_duplicate_inline_submodule_scope_symbol();
  test_compile_sources_resolves_super_qualified_type_paths();
  test_compile_sources_reports_unresolved_qualified_type_path();
  test_compile_sources_reports_unresolved_module_qualified_reference();
  test_compile_sources_reports_unresolved_session_import();
  test_compile_sources_reports_inaccessible_session_import();
  return 0;
}
