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

auto expect(bool condition, std::string_view message) -> void {
  if (!condition) {
    std::cerr << "cli_test failed: " << message << '\n';
    std::exit(1);
  }
}

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

auto test_parse_args_rejects_unknown_options() -> void {
  char arg0[] = "kira";
  char arg1[] = "--bogus";
  char *argv[] = {arg0, arg1};

  auto result = kira::parse_args(2, argv);
  expect(!result.has_value(), "unknown options should fail");
  expect(result.error() == "unknown option: --bogus",
         "expected unknown option error message");
}

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

struct temp_dir {
  fs::path path;

  ~temp_dir() {
    auto ec = std::error_code{};
    fs::remove_all(path, ec);
  }
};

auto make_temp_dir() -> temp_dir {
  auto base = fs::temp_directory_path() /
              std::format("kira_cli_test_{}",
                          std::chrono::steady_clock::now().time_since_epoch().count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, "expected to create temporary test directory");
  return temp_dir{.path = std::move(base)};
}

auto write_file(const fs::path &path, std::string_view contents) -> void {
  auto ec = std::error_code{};
  fs::create_directories(path.parent_path(), ec);
  expect(!ec, "expected to create parent directory for test file");

  auto out = std::ofstream(path, std::ios::binary | std::ios::trunc);
  expect(static_cast<bool>(out), "expected to open test file for writing");
  out << contents;
  expect(out.good(), "expected to write test file contents");
}

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
             "pub def run():\n"
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

} // namespace

auto main() -> int {
  test_parse_args_accepts_sources();
  test_parse_args_supports_help_and_double_dash();
  test_parse_args_accepts_metadata_dir();
  test_parse_args_rejects_unknown_options();
  test_rendering_helpers();
  test_compile_sources_writes_module_metadata();
  test_compile_sources_reports_parser_errors();
  return 0;
}
