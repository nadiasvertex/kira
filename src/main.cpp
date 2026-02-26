#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// A simple demonstration of C++26/23 features:
//   - std::print / std::println  (C++23, widely available in Clang 17)
//   - std::expected              (C++23)
//   - std::string_view literals

struct Config {
    std::string_view program_name;
    std::vector<std::string> args;
};

[[nodiscard]] std::expected<Config, std::string>
parse_args(int argc, char* argv[]) {
    if (argc < 1) {
        return std::unexpected{"argc must be at least 1"};
    }

    Config cfg{
        .program_name = argv[0],
        .args         = {},
    };

    for (int i = 1; i < argc; ++i) {
        cfg.args.emplace_back(argv[i]);
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

    if (cfg.args.empty()) {
        std::println("No arguments provided.");
    } else {
        std::println("Arguments ({}):", cfg.args.size());
        for (std::size_t i = 0; i < cfg.args.size(); ++i) {
            std::println("  [{}] {}", i, cfg.args[i]);
        }
    }

    return 0;
}