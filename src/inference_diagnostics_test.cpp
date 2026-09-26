// The golden corpus of *wrong* programs (`spec/inference-rewrite.md` phase 5).
//
// Every `.cn` here is a program that must be rejected, and its `.expected`
// file holds the compiler's rendered diagnostics for it, byte for byte.
//
// This exists before the blame machinery, not after, and that ordering is the
// whole point. The acceptance bar for phase 5 is "no message may be worse than
// what the same program produces today" — a bar that can only be enforced if
// "today" was written down before anything started changing it. A corpus
// captured afterwards records whatever the new code happens to say, which is
// not a bar at all.
//
// A `.cn` file whose leading comments say `bar: BELOW BAR TODAY` is recording
// a message this compiler is not yet willing to stand behind. Its `.expected`
// text is a floor, not a target: phase 5 is finished when those entries have
// been rewritten deliberately and their goldens updated to match, never when
// they still read the same.
//
// Updating a golden: run with `CINDER_BLESS=1` (the target is `no-sandbox`, so
// it writes back into the source tree), then *read the diff*. A blessed golden
// nobody read is how a regression becomes the expected output.

#include "driver/driver.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

[[noreturn]] auto fail(const std::string &message) -> void {
  std::cerr << "inference_diagnostics_test failed: " << message << '\n';
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
      std::format("cinder_infer_diag_{}",
                  std::chrono::steady_clock::now().time_since_epoch().count());
  auto ec = std::error_code{};
  fs::create_directories(base, ec);
  expect(!ec, std::format("expected to create temporary directory: {}",
                          ec.message()));
  return temp_dir{.path = std::move(base)};
}

auto candidate_corpus_dirs(std::string_view argv0) -> std::vector<fs::path> {
  constexpr auto k_relative = "src/testdata/inference_diagnostics";
  auto candidates = std::vector<fs::path>{};

  if (const auto *srcdir = std::getenv("TEST_SRCDIR"); srcdir != nullptr) {
    if (const auto *workspace = std::getenv("TEST_WORKSPACE");
        workspace != nullptr && *workspace != '\0') {
      candidates.emplace_back(fs::path(srcdir) / workspace / k_relative);
    }
    candidates.emplace_back(fs::path(srcdir) / "_main" / k_relative);
  }

  if (!argv0.empty()) {
    candidates.emplace_back(fs::path(std::string(argv0) + ".runfiles") /
                            "_main" / k_relative);
  }

  candidates.emplace_back(k_relative);
  return candidates;
}

auto find_corpus_dir(std::string_view argv0) -> fs::path {
  for (const auto &candidate : candidate_corpus_dirs(argv0)) {
    auto ec = std::error_code{};
    if (fs::is_directory(candidate, ec)) {
      return candidate;
    }
  }
  fail("could not locate the inference diagnostics corpus directory");
  std::abort();
}

auto list_cases(const fs::path &corpus_dir) -> std::vector<fs::path> {
  auto cases = std::vector<fs::path>{};
  for (const auto &entry : fs::directory_iterator(corpus_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".cn") {
      cases.push_back(entry.path());
    }
  }
  std::ranges::sort(cases);
  return cases;
}

auto read_file(const fs::path &path) -> std::string {
  auto stream = std::ifstream(path, std::ios::binary);
  expect(stream.good(), std::format("expected to read `{}`", path.string()));
  auto buffer = std::ostringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

/// Replaces every occurrence of `absolute` with `replacement`.
///
/// The rendered `-->` lines carry the path the driver was handed, which is
/// absolute and therefore different on every machine. Only the corpus file's
/// own path is rewritten: a path from anywhere else appearing in a golden
/// means a diagnostic escaped into the standard library, which is exactly the
/// failure this corpus is watching for and must not be normalized away.
auto normalize(std::string text, const std::string &absolute,
               std::string_view replacement) -> std::string {
  auto at = text.find(absolute);
  while (at != std::string::npos) {
    text.replace(at, absolute.size(), replacement);
    at = text.find(absolute, at + replacement.size());
  }
  return text;
}

/// Compiles one case and returns its rendered diagnostics, path-normalized.
///
/// `run` stays false: a corpus file has no `main`, and the driver's "no
/// compiled module defines a function named `main`" line would drown the
/// message actually under test.
auto diagnose(const fs::path &source, const fs::path &tmp_dir) -> std::string {
  auto cfg = cinder::driver::cli_config{
      .program_name = "cinder",
      .sources = {source.string()},
      .metadata_dir = (tmp_dir / "meta").string(),
      .show_help = false,
      .run = false,
  };
  cinder::driver::inject_stdlib_prelude(cfg);

  auto report = cinder::driver::compile_sources(cfg, false);
  expect(report.has_value(),
         std::format("[{}] expected compile_sources to return a report",
                     source.filename().string()));

  // Every file in this corpus is a program that must be rejected. A case that
  // stopped producing an error would otherwise "pass" against an empty
  // golden, which is the precise shape of a test that cannot fail.
  expect(report->error_count > 0,
         std::format("[{}] expected the program to be rejected, but it "
                     "compiled cleanly",
                     source.filename().string()));

  return normalize(report->diagnostics, source.string(),
                   source.filename().string());
}

auto blessing() -> bool {
  const auto *flag = std::getenv("CINDER_BLESS");
  return flag != nullptr && *flag != '\0' && std::string_view(flag) != "0";
}

} // namespace

auto main(int argc, char *argv[]) -> int {
  try {
    const auto corpus_dir = find_corpus_dir(argc > 0 ? std::string_view(*argv)
                                                     : std::string_view{});
    const auto cases = list_cases(corpus_dir);
    expect(!cases.empty(),
           "expected the inference diagnostics corpus to contain .cn files");
    expect(cases.size() >= 12,
           std::format("expected the acceptance-bar corpus to be intact, "
                       "found only {} cases",
                       cases.size()));

    auto tmp = make_temp_dir();
    auto blessed = 0;

    for (const auto &source : cases) {
      const auto name = source.filename().string();
      auto golden_path = source;
      golden_path.replace_extension(".expected");

      const auto actual = diagnose(source, tmp.path);
      expect(!actual.empty(),
             std::format("[{}] expected rendered diagnostics", name));

      if (blessing()) {
        auto out = std::ofstream(golden_path, std::ios::binary);
        expect(out.good(),
               std::format("[{}] expected to write the golden", name));
        out << actual;
        ++blessed;
        continue;
      }

      expect(fs::exists(golden_path),
             std::format("[{}] has no `.expected` golden. Every wrong program "
                         "in this corpus records the message it produces; add "
                         "one (CINDER_BLESS=1) and read it before trusting it.",
                         name));

      const auto expected = read_file(golden_path);
      if (actual != expected) {
        std::cerr << std::format(
            "inference_diagnostics_test failed: [{}] diagnostics changed.\n"
            "--- expected ({}) ---\n{}\n--- actual ---\n{}\n"
            "If the new message is better, re-run with CINDER_BLESS=1 and read "
            "the diff before committing it.\n",
            name, golden_path.filename().string(), expected, actual);
        std::exit(1);
      }
    }

    if (blessed > 0) {
      std::cerr << std::format(
          "inference_diagnostics_test: blessed {} golden(s); this run asserted "
          "nothing.\n",
          blessed);
    }
  } catch (const std::exception &ex) {
    std::cerr << "inference_diagnostics_test failed: unhandled exception: "
              << ex.what() << '\n';
    std::exit(1);
  }

  return 0;
}
