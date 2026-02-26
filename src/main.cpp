#include <argparse/argparse.hpp>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

struct Config {
    std::string_view program_name;
    std::vector<std::string> sources;
};

[[nodiscard]] std::expected<Config, std::string>
parse_args(int argc, char* argv[]) {
    if (argc < 1) {
        return std::unexpected{"argc must be at least 1"};
    }

    argparse::ArgumentParser program(argv[0]);

    program.add_description("Kira - A compiler and language processing tool");

    program.add_argument("sources")
        .help("source input file(s) to process")
        .remaining();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        return std::unexpected{err.what()};
    }

    Config cfg{
        .program_name = argv[0],
        .sources      = {},
    };

    try {
        auto sources = program.get<std::vector<std::string>>("sources");
        cfg.sources = sources;
    } catch (const std::logic_error& err) {
        // No sources provided, which is allowed
    }

    return cfg;
}

int main(int argc, char* argv[]) {
    auto result = parse_args(argc, argv);

    if (!result) {
        std::println(stderr, "Error: {}", result.error());
        return 1;
    }

    const auto& cfg = *result;

    std::println("Welcome to Kira!");
    std::println("Invoked as: {}", cfg.program_name);

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