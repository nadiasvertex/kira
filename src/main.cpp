#include <argparse/argparse.hpp>
#include <expected>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

/// @struct Config
/// @brief Configuration structure for the Kira compiler.
///
/// Stores the program name and list of source files to be processed
/// by the Kira compiler.
struct Config {
  std::string_view program_name;    ///< The name of the program executable
  std::vector<std::string> sources; ///< List of source file paths to process
};

/// @brief Parses command-line arguments for the Kira compiler.
///
/// Processes command-line arguments and constructs a Config structure
/// containing the program name and source files to be compiled.
///
/// @param argc The argument count from main()
/// @param argv The argument vector from main()
///
/// @return std::expected<Config, std::string>
///         - On success: a Config structure with parsed arguments
///         - On failure: an error message string describing what went wrong
///
/// @note The function handles:
///       - Help flag (-h, --help) which exits after printing help
///       - Source files passed as remaining arguments
///       - Missing source files (allowed, results in empty sources vector)
///
/// @nodiscard Callers should not ignore the return value without checking for
/// errors
[[nodiscard]] auto parse_args(int argc, char *argv[])
    -> std::expected<Config, std::string> {
  if (argc < 1) {
    return std::unexpected{"argc must be at least 1"};
  }

  argparse::ArgumentParser program(argv[0]);

  program.add_description("Kira - A compiler and language processing tool");

  program.add_argument("-h", "--help")
      .help("show this help message and exit")
      .action([&](const auto &) {
        std::cout << program << std::endl;
        exit(0);
      })
      .append()
      .nargs(0);

  program.add_argument("sources")
      .help("source input file(s) to process")
      .remaining();

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    return std::unexpected{err.what()};
  }

  Config cfg{
      .program_name = argv[0],
      .sources = {},
  };

  try {
    auto sources = program.get<std::vector<std::string>>("sources");
    cfg.sources = sources;
  } catch (const std::logic_error &err) {
    // No sources provided, which is allowed
  }

  return cfg;
}

/// @brief Main entry point for the Kira compiler.
///
/// Initializes the Kira compiler, parses command-line arguments,
/// and displays information about the invocation and source files.
///
/// @param argc The number of command-line arguments
/// @param argv The command-line arguments array
///
/// @return Exit code:
///         - 0 on successful execution
///         - 1 if argument parsing fails
///
/// @note Prints welcome message and lists provided source files
int main(int argc, char *argv[]) {
  auto result = parse_args(argc, argv);

  if (!result) {
    std::println(stderr, "Error: {}", result.error());
    return 1;
  }

  const auto &cfg = *result;

  if (cfg.sources.empty()) {
    std::println("No source files provided.");
  } else {
    std::println("Source file(s) ({}):", cfg.sources.size());
    for (std::size_t i = 0; i < cfg.sources.size(); ++i) {
      std::println("  [{}] {}", i, cfg.sources[i]);
    }
  }

  return 0;
}
