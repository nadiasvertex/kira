#include "cli.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

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
  expect(help.find("Usage: kira [OPTIONS] [SOURCES...]") != std::string::npos,
         "help should contain usage line");
  expect(help.find("Kira - A compiler and language processing tool") !=
             std::string::npos,
         "help should contain description");

  kira::cli_config cfg{
      .program_name = "kira",
      .sources = {"main.kira", "lib.kira"},
      .show_help = false,
  };
  expect(kira::render_source_summary(cfg) ==
             "Source file(s) (2):\n  [0] main.kira\n  [1] lib.kira",
         "expected stable source summary output");

  cfg.sources.clear();
  expect(kira::render_source_summary(cfg) == "No source files provided.",
         "expected empty source summary output");
}

} // namespace

auto main() -> int {
  test_parse_args_accepts_sources();
  test_parse_args_supports_help_and_double_dash();
  test_parse_args_rejects_unknown_options();
  test_rendering_helpers();
  return 0;
}
