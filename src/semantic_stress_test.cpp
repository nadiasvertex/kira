#include "cli.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

[[noreturn]] auto fail(const std::string &message) -> void {
  std::cerr << "semantic_stress_test failed: " << message << '\n';
  std::exit(1);
}

auto expect(bool condition, const std::string &message) -> void {
  if (!condition) {
    fail(message);
  }
}

struct temp_dir {
  fs::path path;

  ~temp_dir() {
    auto ec = std::error_code{};
    fs::remove_all(path, ec);
  }
};

auto make_temp_dir() -> temp_dir {
  auto base =
      fs::temp_directory_path() /
      std::format("kira_semantic_stress_{}",
                  std::chrono::steady_clock::now().time_since_epoch().count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, std::format("expected to create temporary directory: {}",
                          ec.message()));
  return temp_dir{.path = std::move(base)};
}

auto candidate_corpus_dirs(std::string_view argv0) -> std::vector<fs::path> {
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace /
                              "src/testdata/semantic_stress");
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" /
                            "src/testdata/semantic_stress");
  }

  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / "src/testdata/semantic_stress");
  }

  candidates.emplace_back("src/testdata/semantic_stress");
  return candidates;
}

auto find_corpus_dir(std::string_view argv0) -> fs::path {
  for (const auto &candidate : candidate_corpus_dirs(argv0)) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }

  fail("could not locate semantic stress corpus directory");
  std::abort();
}

auto list_corpus_files(const fs::path &corpus_dir) -> std::vector<std::string> {
  auto files = std::vector<std::string>{};
  for (const auto &entry : fs::directory_iterator(corpus_dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() != ".kira") {
      continue;
    }
    files.push_back(entry.path().string());
  }

  std::ranges::sort(files);
  return files;
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    auto corpus_dir = find_corpus_dir(argc > 0 ? std::string_view(*argv)
                                               : std::string_view{});
    auto files = list_corpus_files(corpus_dir);

    expect(!files.empty(),
           "expected semantic stress corpus to contain .kira files");
    expect(files.size() >= 20,
           std::format(
               "expected a broad semantic stress corpus, found only {} files",
               files.size()));

    auto temp = make_temp_dir();
    kira::driver::cli_config cfg{
        .program_name = "kira",
        .sources = files,
        .metadata_dir = temp.path.string(),
        .show_help = false,
        // Unlike the parser stress corpus, these fragments are well-typed and
        // must pass full name resolution and type checking.
        .parse_only = false,
    };

    auto result = kira::driver::compile_sources(cfg, false);
    expect(result.has_value(), "expected compile_sources to return a report");
    auto &report = result.value();
    expect(report.error_count == 0,
           report.diagnostics.empty()
               ? std::string{"expected semantic stress corpus to parse & "
                             "type-check cleanly"}
               : report.diagnostics);
    expect(report.modules.size() == files.size(),
           std::format("expected {} metadata artifacts, got {}", files.size(),
                       report.modules.size()));

    for (const auto &module : report.modules) {
      expect(fs::exists(module.metadata_path),
             std::format("expected metadata artifact `{}` to exist",
                         module.metadata_path));
    }
  } catch (const std::exception &ex) {
    std::cerr << "semantic_stress_test failed: unhandled exception: "
              << ex.what() << '\n';
    std::exit(1);
  }

  return 0;
}
